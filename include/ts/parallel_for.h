#pragma once

#include "ts/scheduler.h"
#include "ts/task.h"
#include "ts/guarded.h"   // global_scheduler

#include <atomic>
#include <concepts>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace ts
{

// How items map to the `concurrency` parallel executors. Only the claim GRAIN differs; the
// worker loop is identical. See docs/task-internals.md.
enum class Balance
{
    guided,       // DEFAULT: grain shrinks as work drains (big early -> low overhead; small late
                  //          -> a straggler is a small chunk others absorb). Best general default.
    balanced,     // fixed coarse chunks (~N/concurrency). Lowest overhead; assumes uniform cost.
    unbalanced,   // grain 1 -- every item claimed separately. Max dynamic balance, max overhead.
};

struct Parallel_options
{
    int     concurrency = 0;                 // # of parallel executors; 0 -> scheduler width
    Balance balance     = Balance::guided;
    // Queue priority for the helper submissions. Unset (default) = inherit: helpers dispatch
    // at the calling task's priority (`Priority::normal` outside a task), so a high-priority
    // node's slices don't sink to `normal`. Set (`{.priority = Priority::low}`) to override.
    // The participating caller is unaffected either way -- it runs inline.
    std::optional<Priority> priority{};
};

namespace detail
{

// Resolve the effective helper priority, on the calling thread (where the `current_task`
// TLS is meaningful): an explicit option wins; otherwise inherit the calling task's queue
// priority; outside a running task, `Priority::normal`.
inline Priority resolved_priority(std::optional<Priority> requested)
{
    if (requested)
        return *requested;
    if (const Task_control_block* c = current_task.get())
        return c->flags.priority;
    return Priority::normal;
}

// The shared, refcounted state for one parallel_for. ONE heap allocation per call; `core`
// (first member) is the completion block the async handle aliases, and its refcount also
// keeps the state alive until every raw helper has exited (so a helper that runs LATE -- a
// queued one that finally gets a worker -- can't touch freed memory). `body` lives here too,
// so it outlives the helpers. See the "no allocs" note in docs/TODO.md (a later pass pools these).
template<typename Body>
struct Parallel_state
{
    Task_control_block core;   // MUST be first; completes when `done == n`
    Body body;
    int n;
    int base_grain;            // ceil(n / concurrency), for `balanced`
    int concurrency;
    Balance balance;
    // The caller's access grant, snapshotted BY VALUE on the calling thread at creation --
    // helpers install it around their loop, so a chunk touching guarded state the caller
    // owns passes the harness on any worker (and, for `async_parallel_for`, even after the
    // caller's own scope unwinds). A by-value grant snapshot, like the coroutine promise's.
    std::optional<Access_context> inherited_ctx;
    int inherited_owner;          // trace owner (graph node index), snapshotted like inherited_ctx
    std::atomic<int> next{ 0 };   // next item index to claim
    std::atomic<int> done{ 0 };   // items processed (acq_rel: publishes body writes to the waiter)

    Parallel_state(Body b, int n_, int conc, Balance bal)
        : body(std::move(b)), n(n_), concurrency(conc), balance(bal)
        , inherited_ctx(snapshot_access())
        , inherited_owner(trace_owner())
    {
        base_grain = (n_ + conc - 1) / conc;
        if (base_grain < 1)
            base_grain = 1;
    }

    // Items to claim on the next fetch_add (approximate for `guided` -- a heuristic, so the
    // racy `next` read is fine; the actual claim clamps to n).
    int grain()
    {
        if (balance == Balance::unbalanced)
            return 1;
        if (balance == Balance::balanced)
            return base_grain;
        int rem = n - next.load(std::memory_order_relaxed);   // guided
        int g = rem / (2 * concurrency);
        return g < 1 ? 1 : g;
    }
};

// The claim + process loop, shared by the raw helpers and the participating (blocking) caller.
template<typename Body>
void parallel_loop(Parallel_state<Body>* st)
{
    for (;;)
    {
        int g = st->grain();
        int start = st->next.fetch_add(g, std::memory_order_relaxed);
        if (start >= st->n)
            break;
        int stop = start + g;
        if (stop > st->n)
            stop = st->n;
        for (int i = start; i < stop; ++i)
            st->body(i);
        // acq_rel forms a release sequence over `done`, so the executor that reaches `n`
        // (and the waiter it wakes) sees every executor's body writes.
        if (st->done.fetch_add(stop - start, std::memory_order_acq_rel) + (stop - start) == st->n)
            st->core.complete();   // processed the last item -> signal completion
    }
}

// Raw scheduler entry for a helper: install the caller's inherited grant, run the loop, then
// drop this helper's refcount (may free the state if the loop already completed and the handle
// is gone). A late-queued helper finds the counter drained, does no work, and just releases
// its ref -- harmless. (The participating CALLER needs no install -- its context is already
// the live one.)
template<typename Body>
void parallel_helper(void* p)
{
    auto* st = static_cast<Parallel_state<Body>*>(p);
    {
        Inherited_access_scope scope(st->inherited_ctx);
        // Attribute this helper's chunk time to the owning graph node (trace true-busy), the
        // same inheritance as the access grant. No-op untraced.
        Trace_owner_scope trace_owner_scope(st->inherited_owner);
        Trace_busy_scope trace_busy_scope;
        parallel_loop(st);
    }
    intrusive_dec(&st->core);
}

// `parallel_for` fans out on the one process-wide `global_scheduler()`. With the single-global
// collapse there is no separate pool a task could belong to -- the running pool IS the global,
// so a graph node's slices and an off-worker caller's slices both land here.
template<typename Body>
Parallel_state<Body>* make_parallel_state(int n, Body&& body, Parallel_options opts, int& conc_out)
{
    int conc = opts.concurrency > 0 ? opts.concurrency : global_scheduler().worker_count();
    if (conc < 1)
        conc = 1;
    if (conc > n)
        conc = n;
    conc_out = conc;

    auto* st = new Parallel_state<Body>(std::forward<Body>(body), n, conc, opts.balance);
    st->core.destroy = [](Task_control_block* c) { delete reinterpret_cast<Parallel_state<Body>*>(c); };
    return st;
}

} // namespace detail

// Run `body(i)` for each i in [0, n), across `opts.concurrency` executors, and BLOCK until all
// items are done. The calling thread participates (runs a share itself) and waits on the
// completion -- so even nested inside a task / another parallel_for it can't deadlock: the
// caller drains all the work itself if no helper gets a worker, then waits only for the
// still-running helpers. `body` must be safe to call concurrently for distinct i. One heap
// allocation (the shared state). `body` is called inline (templated), so per-item is not a cost.
template<typename Body>
    requires std::invocable<Body&, int>
void parallel_for(int n, Body&& body, Parallel_options opts = {})
{
    if (n <= 0)
        return;

    int conc = 0;
    auto* st = detail::make_parallel_state<std::decay_t<Body>>(n, std::forward<Body>(body), opts, conc);

    detail::Task_ptr handle(&st->core);   // the caller's ref (refcount 1)
    st->core.refcount.fetch_add(conc - 1, std::memory_order_relaxed);   // one ref per submitted helper

    Priority prio = detail::resolved_priority(opts.priority);   // resolved once, on the calling thread
    Scheduler& sched = global_scheduler();
    for (int t = 0; t < conc - 1; ++t)
        sched.submit(&detail::parallel_helper<std::decay_t<Body>>, st, prio);

    detail::parallel_loop(st);   // caller participates (its ref is `handle`, so no dec here)
    // The one sanctioned in-task wait (docs/coroutine-first.md §4.1): the caller has already
    // drained every unclaimed chunk itself, so this parks only on helpers that are RUNNING on
    // workers right now -- bounded, deadlock-free (running work always finishes). It waits on
    // the group state directly, never through `sync_wait`, so the in-task blocking fatal
    // does not apply here.
    st->core.wait();             // block until done == n

    // `handle` releases the caller's ref on return; helpers release theirs on exit -> freed.
}

// Non-blocking parallel_for: returns a Task<void> that completes when all items are done.
// Composable (`co_await` it). One heap allocation (the
// returned Task's block holds the state). Do NOT block on it inside a graph node.
template<typename Body>
    requires std::invocable<Body&, int>
Task<void> async_parallel_for(int n, Body&& body, Parallel_options opts = {})
{
    if (n <= 0)
    {
        auto core = detail::make_bare_block();
        core->complete();
        return Task<void>(std::move(core));
    }

    int conc = 0;
    auto* st = detail::make_parallel_state<std::decay_t<Body>>(n, std::forward<Body>(body), opts, conc);

    Task<void> result(detail::Task_ptr(&st->core));   // the returned handle (refcount 1)
    st->core.refcount.fetch_add(conc, std::memory_order_relaxed);   // one ref per helper

    Priority prio = detail::resolved_priority(opts.priority);   // resolved once, on the calling thread
    Scheduler& sched = global_scheduler();
    for (int t = 0; t < conc; ++t)
        sched.submit(&detail::parallel_helper<std::decay_t<Body>>, st, prio);

    return result;
}

} // namespace ts
