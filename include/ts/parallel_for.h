#pragma once

#include "ts/scheduler.h"
#include "ts/task.h"
#include "ts/guarded.h"   // global_scheduler

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace ts
{

// How items map to the `max_workers` parallel executors. Only the claim grain differs; the
// worker loop is identical. See docs/task-internals.md.
enum class Balance
{
    guided,       // default: grain shrinks as work drains (big early -> low overhead; small late
                  //          -> a straggler is a small chunk others absorb). Best general default.
    balanced,     // fixed coarse chunks (~N/max_workers). Lowest overhead; assumes uniform cost.
    unbalanced,   // grain 1 - every item claimed separately. Max dynamic balance, max overhead.
};

struct Parallel_options
{
    // Upper bound on the executors that may participate, the calling thread included (the
    // blocking forms) - helpers are fanned out up to it, and a helper that never gets a worker
    // just finds nothing left to claim. 0 -> the scheduler's worker count.
    int max_workers = 0;
    Balance balance = Balance::guided;
    // Queue priority for the helper submissions. Unset (default) = inherit: helpers dispatch
    // at the calling task's priority (`Priority::normal` outside a task), so a high-priority
    // node's slices don't sink to `normal`. Set (`{.priority = Priority::low}`) to override.
    // The participating caller is unaffected either way - it runs inline.
    std::optional<Priority> priority{};
    // Cancellation for `parallel_for_async` only: once requested, chunks not yet claimed are
    // skipped and the returned task settles cancelled (`Task::is_cancelled()`); chunks already
    // running finish. The blocking forms ignore it - their caller participates and joins, so
    // "stop early" is the body's own early-out, not the loop's.
    Cancellation_token token = {};
};

namespace detail
{

// Items to claim on one fetch/CAS, shared by the flat and colored loops. `claimed` is an
// approximate racy read for `guided` (a heuristic; the actual claim clamps to `size`).
inline int claim_grain(Balance balance, int size, int claimed, int workers)
{
    if (balance == Balance::unbalanced)
        return 1;
    if (balance == Balance::balanced)
    {
        int g = (size + workers - 1) / workers;
        return g < 1 ? 1 : g;
    }
    int g = (size - claimed) / (2 * workers);   // guided
    return g < 1 ? 1 : g;
}

// State shared by every parallel loop flavor: the completion block as a base subobject
// (the handle points at it, and its refcount keeps the state alive until every late
// helper has exited), the body, and the caller's grant + trace-owner snapshots, taken by
// value on the calling thread so a chunk touching guarded state the caller owns passes
// the harness on any worker. One heap allocation per call (see the "no allocs" note in
// docs/TODO.md - a later pass pools these). Derives from `Task_control_block` directly,
// not `Block_backed`: `destroy` must delete the most-derived state
// (`Parallel_state`/`Colored_state`), which `set_destroy` below installs per flavor.
template<typename Body>
struct Parallel_base : Task_control_block
{
    Body body;
    int max_workers;
    Balance balance;
    std::optional<Access_context> inherited_ctx;
    int inherited_owner;       // trace owner (graph node index), snapshotted like inherited_ctx
#if TS_RULES_ANY
    unsigned inherited_relaxed;   // the caller's `Relaxed_scope` opt-outs, snapshotted alike
#endif

    Parallel_base(Body b, int workers, Balance bal)
        : body(std::move(b)), max_workers(workers), balance(bal)
        , inherited_ctx(snapshot_access()), inherited_owner(trace_owner())
#if TS_RULES_ANY
        , inherited_relaxed(snapshot_relaxed())
#endif
    {
    }
};

// --- flat loop (parallel_for) --------------------------------------------------------

template<typename Body>
struct Parallel_state : Parallel_base<Body>
{
    int n;
    std::atomic<int> next{ 0 };   // next item index to claim
    std::atomic<int> done{ 0 };   // items processed or skipped (acq_rel: publishes body writes to the waiter)
    // Set once an executor saw the block's `token` requested and skipped the unclaimed tail;
    // the executor that accounts for the last item then settles the task cancelled. Stored
    // before, and read after, the `done` handoff, so the acq_rel chain carries it. Only
    // `parallel_for_async` installs a token; the blocking forms leave it empty.
    std::atomic<bool> skipped{ false };

    Parallel_state(Body b, int n_, int workers, Balance bal)
        : Parallel_base<Body>(std::move(b), workers, bal), n(n_)
    {
    }
};

// The claim + process loop, shared by the raw helpers and the participating (blocking) caller.
// Accounts every item exactly once through `done`, whether processed or skipped: the executor
// whose accounting reaches `n` settles the task. One relaxed token load per chunk claim, not
// per item (an empty token is a null test).
template<typename Body>
void run_loop(Parallel_state<Body>* st)
{
    for (;;)
    {
        int start, stop;
        if (st->token.is_cancel_requested())
        {
            // Claim everything left in one step and account for it unprocessed.
            start = st->next.exchange(st->n, std::memory_order_relaxed);
            if (start >= st->n)
                break;
            stop = st->n;
            st->skipped.store(true, std::memory_order_relaxed);
        }
        else
        {
            int g = claim_grain(st->balance, st->n, st->next.load(std::memory_order_relaxed), st->max_workers);
            start = st->next.fetch_add(g, std::memory_order_relaxed);
            if (start >= st->n)
                break;
            stop = start + g;
            if (stop > st->n)
                stop = st->n;
            invoke_user_body([&]
            {
                for (int i = start; i < stop; ++i)
                    st->body(i);
            });
        }
        // acq_rel forms a release sequence over `done`, so the executor that reaches `n`
        // (and the waiter it wakes) sees every executor's body writes - and the `skipped` flag.
        if (st->done.fetch_add(stop - start, std::memory_order_acq_rel) + (stop - start) == st->n)
        {
            // Accounted for the last item: settle - cancelled if any chunk was skipped.
            if (st->skipped.load(std::memory_order_relaxed))
                st->cancel();
            else
                st->complete();
        }
    }
}

// --- colored loop (parallel_for_colored, interaction coloring) -----------------------

template<typename Body>
struct Colored_state : Parallel_base<Body>
{
    // A band views the caller's index vector (the call blocks, so the caller's
    // vectors outlive every helper).
    using Band = std::span<const int>;
    std::vector<Band> bands;   // non-empty bands only
    int total_phases = 0;      // rounds x bands
    // `phase << 32 | next-claim-index` in one word: a claim CAS re-checks both halves, so
    // a chunk can never be claimed against a stale phase (the ABA a separate phase counter
    // plus claim counter would allow).
    std::atomic<std::uint64_t> phase_next{ 0 };
    std::atomic<int> band_done{ 0 };

    Colored_state(Body b, int workers, Balance bal) : Parallel_base<Body>(std::move(b), workers, bal) {}
};

// The claim + process + phase-advance loop. No per-band fork/join and no fixed-participant
// barrier: whoever finishes a band's last item advances the phase, so the caller alone can
// drain every phase (helpers only accelerate - `parallel_for`'s deadlock-freedom is kept),
// and a helper that gets a worker late simply joins the current band.
template<typename Body>
void run_loop(Colored_state<Body>* st)
{
    const std::uint64_t terminal = std::uint64_t(st->total_phases) << 32;
    std::uint64_t cur = st->phase_next.load(std::memory_order_acquire);
    for (;;)
    {
        int ph = static_cast<int>(cur >> 32);
        if (ph >= st->total_phases)
            break;
        const auto& band = st->bands[ph % st->bands.size()];
        int band_size = static_cast<int>(band.size());
        int idx = static_cast<int>(cur & 0xffffffffu);
        if (idx >= band_size)
        {
            // Band fully claimed but not yet finished: the remaining chunks are running on
            // workers right now, so this wait is bounded - the same exemption as
            // `parallel_for`'s join. Spin briefly (the flip is usually imminent), then park
            // on the atomic: drained participants cost nothing while the last chunks run,
            // and a sanitizer sees a futex wait instead of an instrumented spin storm (the
            // yield-spin version starved the runner under TSan and tripped the watchdog).
            std::uint64_t seen = cur;
            int spins = 0;
            while ((seen = st->phase_next.load(std::memory_order_acquire)) == cur)
            {
                if (++spins > 64)
                {
                    st->phase_next.wait(cur, std::memory_order_acquire);
                    spins = 0;
                }
            }
            cur = seen;
            continue;
        }
        int g = claim_grain(st->balance, band_size, idx, st->max_workers);
        int stop = idx + g;
        if (stop > band_size)
            stop = band_size;
        if (!st->phase_next.compare_exchange_weak(cur, (std::uint64_t(ph) << 32) | std::uint64_t(stop),
                std::memory_order_acq_rel, std::memory_order_acquire))
            continue;   // cur reloaded by the failed CAS
        invoke_user_body([&]
        {
            for (int i = idx; i < stop; ++i)
                st->body(band[i]);
        });
        // acq_rel: the last finisher acquires every sibling's body writes here, and its
        // release-store of the new phase below publishes them to the next band's claimers.
        if (st->band_done.fetch_add(stop - idx, std::memory_order_acq_rel) + (stop - idx) == band_size)
        {
            if (ph + 1 >= st->total_phases)
            {
                st->phase_next.store(terminal, std::memory_order_release);
                st->phase_next.notify_all();
                st->complete();
                break;
            }
            st->band_done.store(0, std::memory_order_relaxed);   // ordered by the release below
            cur = std::uint64_t(ph + 1) << 32;
            st->phase_next.store(cur, std::memory_order_release);
            st->phase_next.notify_all();
            continue;
        }
        cur = st->phase_next.load(std::memory_order_acquire);
    }
}

// --- shared scaffolding --------------------------------------------------------------

// Raw scheduler entry for a helper of either flavor: install the caller's inherited grant,
// relaxation and trace owner, run the flavor's loop, then drop this helper's refcount (may
// free the state if the work already completed and the handle is gone). A late-queued helper
// finds nothing to claim (or the terminal phase), does no work, and just releases its ref.
// (The participating caller needs no install - its context is already the live one.)
template<typename State>
void helper_entry(void* p)
{
    auto* st = static_cast<State*>(p);
    {
        Inherited_access_scope scope(st->inherited_ctx);
        // A helper runs the caller's body inside the caller's grant window, so it also runs
        // under the caller's rule opt-outs - otherwise enforcement would depend on which
        // chunk a thread claimed, since the caller's own share runs inline and relaxed.
#if TS_RULES_ANY
        Inherited_relaxed_scope relaxed_scope(st->inherited_relaxed);
#endif
        // Attribute this helper's chunk time to the owning graph node (trace true-busy),
        // the same inheritance as the access grant. No-op untraced.
        Trace_owner_scope trace_owner_scope(st->inherited_owner);
        Trace_busy_scope trace_busy_scope;
        run_loop(st);
    }
    intrusive_dec(st);
}

// Installs the deleter for the most-derived `State` (the block's `destroy` receives the
// base pointer; the `static_cast` recovers the state, `Block_backed`-style).
template<typename State>
void set_destroy(State* st)
{
    st->destroy = [](Task_control_block* c) { delete static_cast<State*>(c); };
}

// The effective executor count: the option, else the scheduler's worker count, clamped to
// [1, `items`] (no executor could ever claim beyond the work).
inline int effective_workers(int max_workers, int items)
{
    int workers = max_workers > 0 ? max_workers : global_scheduler().worker_count();
    if (workers < 1)
        workers = 1;
    if (workers > items)
        workers = items;
    return workers;
}

// Fan out `max_workers - 1` helpers on the one process-wide `global_scheduler()` (with the
// single-global collapse there is no other pool a task could belong to), participate on
// the calling thread, then wait out the still-running helpers. The wait is the one
// sanctioned in-task block (docs/coroutine-first.md §4.1): the caller has already drained
// every unclaimed chunk itself, so it parks only on chunks running on workers right now -
// bounded, deadlock-free. It waits on the group state directly, never through
// `sync_wait`, so the in-task blocking fatal does not apply.
template<typename State>
void run_participants(State* st, std::optional<Priority> priority)
{
    Task_ptr handle(st);   // the caller's ref (refcount 1)
    int workers = st->max_workers;
    st->refcount.fetch_add(workers - 1, std::memory_order_relaxed);   // one ref per helper

    Priority prio = resolved_priority(priority);   // resolved once, on the calling thread
    Scheduler& sched = global_scheduler();
    for (int t = 0; t < workers - 1; ++t)
        sched.submit(&helper_entry<State>, st, prio);

    run_loop(st);      // caller participates (its ref is `handle`, so no dec here)
    st->wait();        // block until complete

    // `handle` releases the caller's ref on return; helpers release theirs on exit -> freed.
}

} // namespace detail

// Run `body(i)` for each i in [0, n), across up to `opts.max_workers` executors, and block until
// all items are done. The calling thread participates (runs a share itself) and waits on the
// completion - so even nested inside a task / another parallel_for it can't deadlock: the
// caller drains all the work itself if no helper gets a worker, then waits only for the
// still-running helpers. `body` must be safe to call concurrently for distinct i. One heap
// allocation (the shared state). `body` is called inline (templated), so per-item is not a cost.
// `opts.token` is ignored (see `Parallel_options`).
template<typename Body>
    requires std::invocable<Body&, int>
void parallel_for(int n, Body&& body, Parallel_options opts = {})
{
    if (n <= 0)
        return;

    int workers = detail::effective_workers(opts.max_workers, n);
    auto* st = new detail::Parallel_state<std::decay_t<Body>>(std::forward<Body>(body), n, workers, opts.balance);
    detail::set_destroy(st);
    detail::run_participants(st, opts.priority);
}

// Non-blocking parallel_for: returns a Task<void> that completes when all items are done.
// Composable (`co_await` it). One heap allocation (the returned Task's block holds the state).
// Do not block on it inside a graph node. `opts.token` cancels it: chunks not yet claimed when
// the request lands are skipped and the task settles cancelled (a cancelled `void` task awaits
// and syncs normally; `is_cancelled()` tells). An empty token cannot be requested, so the loop
// reads as before.
template<typename Body>
    requires std::invocable<Body&, int>
[[nodiscard("nothing else joins the slices: co_await or sync the returned task "
            "(the blocking parallel_for joins for you)")]]
Task<void> parallel_for_async(int n, Body&& body, Parallel_options opts = {})
{
    if (n <= 0)
    {
        auto core = detail::make_bare_block();
        core->complete();
        return Task<void>(std::move(core));
    }

    int workers = detail::effective_workers(opts.max_workers, n);
    using State = detail::Parallel_state<std::decay_t<Body>>;
    auto* st = new State(std::forward<Body>(body), n, workers, opts.balance);
    detail::set_destroy(st);
    st->token = std::move(opts.token);   // read by the helpers' `run_loop`; the block's own slot

    Task<void> result(detail::Task_ptr{ st });   // the returned handle (refcount 1)
    st->refcount.fetch_add(workers, std::memory_order_relaxed);   // one ref per helper

    Priority prio = detail::resolved_priority(opts.priority);   // resolved once, on the calling thread
    Scheduler& sched = global_scheduler();
    for (int t = 0; t < workers; ++t)
        sched.submit(&detail::helper_entry<State>, st, prio);

    return result;
}

// Colored parallel loop - the execution engine for interaction coloring (items grouped
// into bands whose members touch disjoint state; see `sample/coloring.cpp` and
// docs/pattern-farming.md 2.39). Runs `rounds` passes over `bands`, bands sequential
// within a pass, one band's items in parallel: `body(item)` for every item stored in the
// band. Helpers are fanned out once for the whole rounds x bands run; band transitions are
// atomic phase advances, not per-band fork/join. Blocks until done (the `parallel_for`
// join exemption). If the bands uphold the coloring invariant (no two items of one band
// write shared state), the result is independent of chunking, stealing, and worker count -
// bit-deterministic. The band vectors must stay alive and unchanged for the duration of
// the call (it blocks, so a caller-owned coloring is naturally sufficient).
template<typename Body>
    requires std::invocable<Body&, int>
void parallel_for_colored(const std::vector<std::vector<int>>& bands, int rounds, Body&& body,
    Parallel_options opts = {})
{
    int widest = 0;
    for (const std::vector<int>& band : bands)
        widest = std::max(widest, static_cast<int>(band.size()));
    if (rounds <= 0 || widest == 0)
        return;

    int workers = detail::effective_workers(opts.max_workers, widest);   // clamped to the widest band

    using State = detail::Colored_state<std::decay_t<Body>>;
    auto* st = new State(std::forward<Body>(body), workers, opts.balance);
    detail::set_destroy(st);
    for (const std::vector<int>& band : bands)
    {
        if (!band.empty())
            st->bands.emplace_back(band);
    }
    st->total_phases = rounds * static_cast<int>(st->bands.size());

    detail::run_participants(st, opts.priority);
}

} // namespace ts
