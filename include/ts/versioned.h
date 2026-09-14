#pragma once

#include "ts/access.h"
#include "ts/coroutine_support.h"   // the held-grant awaiter `read_last_versions` resumes through
#include "ts/fatal.h"
#include "ts/guarded.h"
#include "ts/recorder.h"
#include "ts/task.h"

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <source_location>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ts
{

// How `publish()` brings the shadow replica back in sync after the swap (both
// replicas must be identical at the start of every publish - the invariant that
// makes staged deltas equivalent to full state).
enum class Resync
{
    // Re-apply the same batch to the new shadow (the old front). Both applications
    // see bit-identical pre-states, so deterministic commands produce bit-identical
    // replicas - cost proportional to the delta, not to sizeof(T). Requires
    // commands to be deterministic (capture RNG/time/etc. results at stage time,
    // never read them inside the closure). The default.
    replay,

    // Copy front over shadow after the swap (user fn via `set_copy`, or T's copy
    // assignment). For nondeterministic commands or cheap-to-copy T. Cost
    // proportional to the state.
    copy,

    // No resync: the user contract is that every version's staged writes fully
    // overwrite the state (per-frame event lists, cleared and refilled). Partial
    // writes under this policy read stale data - that is the user's assertion.
    overwrite,
};

// Which published versions a `Versioned` keeps readable. `current` is the double buffer.
// `current_and_previous` also keeps the version published before it, readable together with
// the current one through `read_last_versions` - what a consumer interpolating between a
// fixed-rate producer's outputs needs. The cost is a third replica, rotated at every publish,
// and a resync by copy: after the rotation the shadow holds the version before last, which one
// replayed batch cannot bring forward, so `Resync::replay` is rejected (`copy` is the default for
// this history, `overwrite` is allowed).
enum class History { current, current_and_previous };

// The publish instants of the two versions `read_last_versions` returns.
struct Version_stamps
{
    std::chrono::steady_clock::time_point previous_published{};
    std::chrono::steady_clock::time_point current_published{};
    std::uint64_t current_serial = 0;   // publishes so far - 0 before the first

    // Where `at` falls between the two publishes: 0 at `previous_published`, 1 at
    // `current_published`, clamped to [0, 1]. 1 when the two coincide (before the second publish).
    double fraction_at(std::chrono::steady_clock::time_point at) const noexcept;
};

template<typename T, History history = History::current>
class Versioned;

namespace detail
{

template<typename T> struct Version_awaiter;
struct Versioned_access;

// The storage `History::current_and_previous` adds; empty otherwise (a base, so it costs nothing).
template<typename T, History history>
struct Versioned_history
{
};

template<typename T>
struct Versioned_history<T, History::current_and_previous>
{
    T previous_{};              // the version published before the front's, rotated in at every swap
    Version_stamps stamps_{};   // written under the front's write grant, read under a read grant
};

// `read_last_versions()` from a task that holds no grant on the front would park a worker on
// the front's pipe - the in-task blocking rule, with the awaitable form as the fix.
inline void check_version_read_may_block()
{
#if TS_RULE_ON(TS_RULE_IN_TASK_SYNC)
    if (Current_task::get() != nullptr && rule_enforced(Rule::in_task_sync))
    {
        ts::fatal("Versioned::read_last_versions() inside a task that holds no grant on the front would block "
                  "a worker - declare state() on the node, or co_await ts::read_last_versions(v)");
    }
#endif
}

} // namespace detail

// The last two published versions of a `Versioned<T, History::current_and_previous>`, held
// under one read grant on its front for the view's lifetime:
//
//   auto [previous, current, stamps] = poses.read_last_versions();
//   render(lerp(previous, current, stamps.fraction_at(now)));
//
// Non-copyable and non-movable: the view is the grant, and it installs its own access context
// (like `Access_guard`), so both versions pass the harness while it lives and neither does
// after. In a coroutine it counts as a live guard: `co_await` while one is alive is fatal.
template<typename T>
class Version_view
{
public:
    const T& previous;
    const T& current;
    const Version_stamps stamps;

    ~Version_view();

    Version_view(const Version_view&) = delete;
    Version_view& operator=(const Version_view&) = delete;

    // Tuple protocol (`std::tuple_size`/`tuple_element` below): 0 = previous, 1 = current,
    // 2 = stamps.
    template<std::size_t I>
    decltype(auto) get() const noexcept;

private:
    template<typename U, History> friend class Versioned;
    friend struct detail::Version_awaiter<T>;

    // `held_pipe` is the front's pipe when the view took its own read turn (released at
    // destruction), null when the calling context already granted the front (the view is lent).
    Version_view(const T& previous_version, const T& current_version, const Version_stamps& stamps_now,
                 detail::Pipe* held_pipe) noexcept;

    detail::Pipe* held_pipe_;
    Access_context ctx_;
    const Access_context* prev_ = nullptr;
    bool counted_ = false;   // counted as a live guard of the running coroutine
};

// `Versioned<T>` - double-buffered state with an atomic publish step: a coarse, batched
// cousin of RCU / MVCC snapshot isolation. It keeps two copies of `T` behind one
// `Guarded<T>` "front": readers always see the last published version - a stable
// snapshot, taken without locking the writer - while a producer prepares the next.
// Writes surface at the next `publish()`, not immediately: a one-version lag. You
// typically publish once per update cycle (a frame, in a game loop) but may publish as
// often as you version.
//
// Producers `stage()` closures into a private journal (grant-free, no contention between
// producers). `publish()` prepares the next version off to the side, then briefly takes
// exclusive (write) access to the front to swap it in - readers of the previous version
// finish first, so the exclusive window is tiny. Access control is `Guarded`'s: readers
// declare an ordinary read on the front (`state()`), so the harness and any
// `Static_task_graph` treat it like a normal guarded object. (Swap/resync mechanics:
// `docs/internals/deferred-versioned-state.md`.)
//
// `Versioned<T, History::current_and_previous>` keeps the version before the current one as
// well, readable as a pair through `read_last_versions` (see `History`).
//
// Use (dynamic tasks):
//   ts::Versioned<Transforms> tf{ ts::Named{"transforms"} };  // owns both replicas
//   ts::Guarded<Transforms>& front = tf.state();     // its front - a Guarded readers access
//   auto rec = tf.recorder();
//   rec.stage([b = std::move(out)](Transforms& t){ t.apply(b); });  // stage next version
//   co_await tf.publish();                           // or sync() it from a blue thread
//   co_await tf.read([](const Transforms& t){ render(t); });        // read the current version
// Composes with `Static_task_graph`: a `ts::publish_fn(tf)` node is the flip; declaring a
// read on the front before it reads the previous version, after it the fresh one. see
// game_frame.cpp sample.
//
// Contract:
//  - No read-your-writes: `read()` sees the last published version; staged writes appear
//    only at the next `publish()`.
//  - One publisher at a time - sync the `publish()`, or order it before a run. A
//    graph/inline publish that catches an unresolved dynamic one is fatal.
//  - Under `replay` resync (the default) commands must be deterministic, so both copies
//    converge. If yours can't be, set the policy to `copy` or `overwrite` (no
//    re-execution, so no determinism needed). `set_divergence_check()` flags a mismatch
//    in dev.
//  - Sync the task `publish()` returns before destroying; leftover staged commands at
//    destruction are fatal lost writes (`discard()` drops them).
//
// Not the right tool when you need a write visible in the same version, or when producing
// the next version is heavy compute rather than a data delta - then keep the machine in
// one sealed `Guarded` and version its output extract instead (see `sample/physics.cpp`).
//
// Sibling `Deferred<T>` shares the same staging journal but applies to a single object
// with no second replica or snapshot - reach for `Deferred<T>` to batch writes and apply
// them at a chosen point, `Versioned<T>` when readers need a stable snapshot across a cycle.
template<typename T, History history>
class Versioned : private detail::Versioned_history<T, history>
{
    static_assert(std::default_initializable<T>, "Versioned<T>: T must be default-constructible (both replicas)");
    static_assert(std::swappable<T>, "Versioned<T>: publish swaps the replicas' contents");

    friend struct detail::Versioned_access;

    // Replay cannot resync the rotated shadow of `History::current_and_previous` (see `History`).
    static constexpr Resync default_resync = history == History::current ? Resync::replay : Resync::copy;

public:
    // Leading `ts::Named` (a literal, or `ts::Named{}` for the construction site) names the
    // front instance for the DOT dump, the trace and the diagnostics. Required, like
    // `Guarded`'s: the name is what every diagnostic about this object prints.
    template<typename N>
        requires std::same_as<std::remove_cvref_t<N>, Named>
    explicit Versioned(N&& name, Resync policy = default_resync)
        : front_(name)
        , policy_(policy)
        , front_ptr_(detail::Guarded_access::instance(front_))
    {
        init();
    }

    // With a declared lock rank (`ts::Rank`, access.h), forwarded to the front `Guarded`:
    // required only when the front is dynamically awaited (`co_await ts::read_only(v.state())`)
    // while another grant is held. Without a rank the `access_rank` rule has no order to check
    // against and such an await cannot be satisfied - graph declarations and `read()` need none.
    template<typename N>
        requires std::same_as<std::remove_cvref_t<N>, Named>
    explicit Versioned(N&& name, Rank rank, Resync policy = default_resync)
        : front_(name, rank)
        , policy_(policy)
        , front_ptr_(detail::Guarded_access::instance(front_))
    {
        init();
    }

    // Two destruction contracts, both fatal under TS_SAFETY_CHECKS (same severity as
    // an undeclared access): destroying with staged-but-unpublished commands (lost
    // writes - `discard()` is the escape), and destroying while a publish this
    // Versioned issued is still in flight (sync the task `publish()` returned first).
    ~Versioned()
    {
        Task<void> chain;
#if TS_SAFETY_CHECKS
        Task<void> last_publish;
#endif
        {
            std::lock_guard lock(seq_mutex_);
#if TS_SAFETY_CHECKS
            last_publish = last_publish_;
#endif
            chain = chain_;
        }
#if TS_SAFETY_CHECKS
        // The task `publish()` returns settles at the swap; if it has not, a publish is
        // genuinely mid-flight and the destructor would otherwise block silently -
        // surface it. (The resync tail can still be pending after a correctly-synced
        // publish; the wait below covers that and is not this check's concern.)
        if (!last_publish.is_done())
        {
            fatal("Versioned destroyed with a publish still in flight - sync the task "
                  "publish() returned before destroying the Versioned");
        }
#endif
        // Lifetime, all builds: the publish's phases and the resync job reference this
        // Versioned; wait them out so none runs against a destroyed one. A no-op once
        // the last publish is fully resolved.
        chain.sync();
        detail::Guarded_access::pipe(front_).wait_until_idle();
#if TS_SAFETY_CHECKS
        if (journal_.has_staged())
            fatal("Versioned destroyed with staged unpublished commands (lost writes); publish or discard() first");
#endif
    }

    Versioned(const Versioned&) = delete;
    Versioned& operator=(const Versioned&) = delete;

    // Producer handle; staging never touches either replica (no grant, no
    // contention with readers or other recorders). Apply order: recorder creation
    // order, FIFO within a recorder.
    Recorder<T> recorder()
    {
        return Recorder<T>(journal_, journal_.add_slot());
    }

    // Per-worker handle for parallel staging (see `Parallel_recorder` in
    // journal.h). Mint once, reuse; cross-thread placement order is
    // nondeterministic, so commands should be per-key single or commutative -
    // the batch order is still fixed at the cut, so `replay` resync stays exact.
    Parallel_recorder<T> parallel_recorder()
    {
        return Parallel_recorder<T>(journal_, global_scheduler());
    }

    // Read the current published version - an ordinary read access on the front
    // (concurrent readers overlap; a queued publish orders around it, FIFO). Returns the
    // caller-owned `Access_op` (the attended `access` verb): the front is read-mostly by
    // design - readers share it and the writer holds it for one ~ns swap per cycle - so the
    // inline arm hits nearly always and a read is zero-alloc. Attended also makes
    // copy-out-and-discard correct by construction (the op temporary's destructor waits out
    // the settle; the old detached form let a discarded read task race its side effects).
    // For a leaver - storing the handle, walking away - use the detached spelling:
    // `v.state().async(fn)`.
    template<typename Fn>
        requires detail::Read_only_accessor<Fn, T>
    [[nodiscard("the attended verb: consume the op (co_await, .sync(), try_take()). To not wait, use v.state().async(fn)")]]
    auto read(Fn&& fn, Access_options opts = {},
              std::source_location site = std::source_location::current()) const
    {
        return std::as_const(front_).access(std::forward<Fn>(fn), opts, site);
    }

    // The last two published versions under one read grant - see `Version_view`. Lent when
    // the calling context already grants the front (a graph node that declared `state()`, a
    // body under `read_only`), so no turn is taken. On a blue thread it takes a read turn,
    // parking behind a writer. A task that holds no grant on the front uses the awaitable
    // `co_await ts::read_last_versions(v)`; blocking a worker here is fatal under
    // `Rule::in_task_sync`.
    [[nodiscard("the view is the read grant: bind it - auto [previous, current, stamps] = ...")]]
    Version_view<T> read_last_versions()
        requires (history == History::current_and_previous);

    // The front's `Guarded` - for static-graph declarations. Declare read access
    // only; the one sanctioned writer is the publish node (`publish_fn`).
    // Writing it directly bypasses versioning and breaks the replica invariant.
    Guarded<T>& state() { return front_; }
    const Guarded<T>& state() const { return front_; }

    // Publish everything staged so far as one atomic version step. The returned
    // task completes at the swap (phase 2) - order fresh readers after it; the
    // resync continues past it, invisibly. An empty journal is a no-op. A
    // cancelled `opts.token` skips the step (commands stay staged for the next
    // publish); the returned task still completes - it is a phase gate, not the
    // skipped work itself. `opts.name` identifies the step - the returned gate and the swap
    // access both carry it; left empty, the call site this verb captures does (`site` is the
    // naming boundary, ts/named.h).
    [[nodiscard("await or sync the publish: ~Versioned is fatal while the swap is in flight")]]
    Task<void> publish(Dispatch_options opts = {},
                       std::source_location site = std::source_location::current())
    {
        const Named name = detail::named_from(opts, site);
        Signal swapped;        // the returned handle: version visible
        Signal shadow_ready;   // gates the NEXT publish's phase 1
        detail::set_task_name(detail::core_of(swapped), name);

        Task<void> prev;
        {
            std::lock_guard lock(seq_mutex_);
            prev = std::exchange(chain_, shadow_ready);
#if TS_SAFETY_CHECKS
            last_publish_ = swapped;   // the returned gate - the destructor's in-flight check
#endif
        }

        // Phase 1 runs once the previous publish's resync is done (shadow free). Wired at
        // the detail level (a waiter on `prev`'s block that submits phase 1 as a task at
        // the publish's priority): the public continuation verb is gone (coroutine-first),
        // and this chain runs on blue threads, so it cannot await.
        // Resolved here, on the publishing thread: phase 1 runs later on whichever thread settles the
        // previous resync, where "the calling task" would mean the wrong one.
        Priority priority = detail::resolved_priority(opts.priority);
        auto phase1 = [this, swapped, shadow_ready, opts, name, priority]() mutable
        {
            if (opts.token.is_cancel_requested())
            {
                swapped.trigger();
                shadow_ready.trigger();
                return;
            }
            journal_.cut(batch_);   // in-place: reuses the batch slot's buffer
            if (batch_.empty())
            {
                swapped.trigger();   // readers keep the current version, which equals the would-be next
                shadow_ready.trigger();
                return;
            }
            apply_to_shadow(batch_);

            // Phase 2: the only write access - swap and get out. The resync is
            // submitted before the phase gate triggers: anything ordered after the
            // returned task (a sync(), an .after()) then finds the resync's read
            // already submitted, so a following writer - including a graph flip's
            // acquire - FIFO-orders behind it. The flip-entry enforcement
            // check relies on exactly this.
            front_.async([this, swapped, shadow_ready](T& front) mutable
            {
                swap_replicas(front);
                start_resync(std::move(shadow_ready));
                swapped.trigger();
            }, { .priority = priority, .name = name });
        };
        detail::core_of(prev)->attach(
            [phase1 = std::move(phase1), priority, name](void*, bool) mutable
            {
                auto core = detail::make_executable<void>(std::move(phase1), {});
                core->flags.priority = priority;
                detail::set_task_name(core, name);
                detail::submit_ready(core);
            });

        return swapped;
    }

    // Publish under a write grant the caller already holds - the graph-node form
    // (see `publish_fn`). `front` must be this Versioned's front instance.
    // Phases 1-2 run inline (the node holds the grant regardless; edges already
    // order readers around it); phase 3 is deferred to the resync read access, admitted
    // the moment the node releases its objects.
    void publish_into(T& front)
    {
#if TS_SAFETY_CHECKS
        if (&front != front_ptr_)
            fatal("Versioned::publish_into: not this Versioned's front instance");
        access_check(&front);
#endif
        // Enforcement + chain handoff, atomically at entry. A dynamic publish
        // whose phase 1 has not been submitted yet is invisible to this node's
        // acquire - proceeding would race its shadow apply and orphan its
        // chain signal. Fatal is the only correct response (a node must never
        // block). Installing our signal here also makes the other direction
        // legal: a dynamic publish arriving mid-flip chains behind us.
        Signal shadow_ready;
        {
            std::lock_guard lock(seq_mutex_);
#if TS_SAFETY_CHECKS
            if (!chain_.is_done())
            {
                fatal("Versioned: graph/inline publish while a dynamic publish is unresolved - "
                      "one publisher at a time; sync() the publish or order it before the run");
            }
#endif
            chain_ = shadow_ready;
        }

        journal_.cut(batch_);   // in-place: reuses the batch slot's buffer
        if (batch_.empty())
        {
            shadow_ready.trigger();   // the chain must still resolve
            return;
        }

        apply_to_shadow(batch_);
        swap_replicas(front);
        start_resync(std::move(shadow_ready));
    }

    // Custom copy for `Resync::copy` (dst = shadow, src = front). Optional when T
    // is copy-assignable.
    void set_copy(std::function<void(T& dst, const T& src)> copy)
    {
        copy_ = std::move(copy);
    }

    // Opt-in replay verification (TS_SAFETY_CHECKS): after every replay resync,
    // hash both replicas and fatal on mismatch. Bitwise equality is the right
    // check - replay-twice has no FP drift (same commands, same order, same
    // pre-state, same binary). Partial hashes (hot arrays only) are fine.
    void set_divergence_check(std::function<std::size_t(const T&)> hash)
    {
        hash_ = std::move(hash);
    }

    // Drop everything staged so far (teardown escape hatch).
    void discard()
    {
        journal_.clear_staged();
    }

private:
    using Batch = std::vector<typename detail::Journal<T>::Command>;

    // The construction tail both constructors share: a resolved publish chain, and for
    // `History::current_and_previous` the resync check and the initial stamps.
    void init()
    {
        Signal ready;
        ready.trigger();
        chain_ = ready;          // the "previous publish" of the first publish
#if TS_SAFETY_CHECKS
        last_publish_ = ready;   // the "last" publish's returned gate - done for a fresh instance
#endif
        if constexpr (history == History::current_and_previous)
        {
            if (policy_ == Resync::replay)
            {
                fatal("Versioned<T, History::current_and_previous> with Resync::replay - after the rotation the "
                      "shadow holds the version before last, which one replayed batch cannot bring forward; "
                      "use Resync::copy (the default for this history) or Resync::overwrite");
            }
            const auto now = std::chrono::steady_clock::now();
            this->stamps_.previous_published = now;
            this->stamps_.current_published = now;
        }
    }

    // Phase 1 work: the shadow is unobservable, so this needs no grant on the
    // front - readers of the current version run concurrently.
    void apply_to_shadow(Batch& batch)
    {
        Access_context ctx;
        ctx.add(&shadow_, Access::read_write);
        Access_scope scope(ctx);
        for (auto& cmd : batch)
            cmd(shadow_);
    }

    // Phase 2 work: nanoseconds under the write grant. The scope names every
    // replica it touches in case T's swap runs instrumented members.
    void swap_replicas(T& front)
    {
        Access_context ctx;
        ctx.add(&front, Access::read_write, detail::pipe_epoch(detail::Guarded_access::pipe(front_)), detail::pipe_rank(detail::Guarded_access::pipe(front_)));
        ctx.add(&shadow_, Access::read_write);   // shadow: no pipe - grant-free by design
        if constexpr (history == History::current_and_previous)
            ctx.add(&this->previous_, Access::read_write);   // no pipe either: the front's grant covers it
        Access_scope scope(ctx);
        using std::swap;
        swap(front, shadow_);
        if constexpr (history == History::current_and_previous)
        {
            // Rotate: the old front becomes the previous version, and the old previous becomes
            // the shadow the copy resync then brings to the new version.
            swap(shadow_, this->previous_);
            this->stamps_.previous_published = this->stamps_.current_published;
            this->stamps_.current_published = std::chrono::steady_clock::now();
            ++this->stamps_.current_serial;
        }
    }

    // Phase 3: bring the new shadow (old front contents) to the new version, as a
    // read job on the front's pipe - overlapping readers, FIFO-ordered before the
    // next writer. `overwrite` needs no work (and no job). The batch lives in the
    // member slot (`batch_`); this job is its last reader and clears it before
    // triggering `shadow_ready` - the trigger is what licenses the next publish's
    // cut into the slot.
    void start_resync(Signal shadow_ready)
    {
        if (policy_ == Resync::overwrite)
        {
            batch_.clear();   // last use was the phase-1 apply; commands die with the cycle
            shadow_ready.trigger();
            return;
        }
        std::as_const(front_).async([this, shadow_ready](const T& front) mutable
        {
            {
                Access_context ctx;
                ctx.add(&shadow_, Access::read_write);   // shadow: no pipe - grant-free by design
                ctx.add(&front, Access::read_only, detail::pipe_epoch(detail::Guarded_access::pipe(front_)), detail::pipe_rank(detail::Guarded_access::pipe(front_)));
                Access_scope scope(ctx);
                if (policy_ == Resync::replay)
                {
                    // Both replicas held version N-1 when the batch was cut, so
                    // this second application sees the same pre-state as the first
                    // - deterministic commands land both replicas at bit-identical
                    // version N.
                    for (auto& cmd : batch_)
                        cmd(shadow_);
                }
                else   // Resync::copy
                {
                    if (copy_)
                        copy_(shadow_, front);
                    else if constexpr (std::is_copy_assignable_v<T>)
                        shadow_ = front;
                    else
                        fatal("Versioned<T>: Resync::copy needs set_copy() for a non-copy-assignable T");
                }
#if TS_SAFETY_CHECKS
                if (hash_ && policy_ == Resync::replay && hash_(front) != hash_(shadow_))
                    fatal("Versioned<T>: replica divergence after replay resync - a staged command is nondeterministic");
#endif
            }
            batch_.clear();   // commands (and their captures) die with the cycle; capacity retained
            shadow_ready.trigger();
        });
    }

    Guarded<T> front_;                // readers' pipe + the published replica
    T shadow_{};                      // the replica the next version is built in (value-init: must equal the front's initial state)
    detail::Journal<T> journal_;
    // The in-flight publish's batch. ONE member slot suffices because the publish chain
    // serializes batch lifetimes: `shadow_ready` triggers only after the batch's last use
    // (the resync's replay for replay/copy, the phase-1 apply for overwrite, the cut itself
    // for the empty early-out; a cancelled publish never cuts), and the next publish's phase
    // 1 - the next `journal_.cut(batch_)` into this slot - gates on exactly that signal
    // (`chain_`; a flip enforces `chain_.is_done()` at entry). The in-place cut clears and
    // refills this buffer rather than move-assigning a fresh one, so the vector's capacity
    // is genuinely reused across publishes (steady-state publishes allocate no batch); the
    // last reader clears it, so captured resources die with the cycle. This replaced a
    // make_shared<Batch> per publish. The destructor's chain sync + pipe drain covers the
    // resync job's `this` capture.
    Batch batch_;
    Resync policy_;
    T* front_ptr_;
    std::mutex seq_mutex_;            // guards `chain_` handoff between publishes
    Task<void> chain_;                // the latest publish's shadow_ready - the next phase 1 gates on it
#if TS_SAFETY_CHECKS
    Task<void> last_publish_;         // the latest publish()'s returned swap-gate - the destructor's in-flight check (safety-only)
#endif
    std::function<void(T&, const T&)> copy_;
    std::function<std::size_t(const T&)> hash_;
};

namespace detail
{

// Reaches the storage `History::current_and_previous` adds, for the free `read_last_versions`.
struct Versioned_access
{
    template<typename T, History history>
    static T* front(Versioned<T, history>& v) noexcept { return v.front_ptr_; }

    template<typename T, History history>
    static const T& previous(const Versioned<T, history>& v) noexcept { return v.previous_; }

    template<typename T, History history>
    static const Version_stamps& stamps(const Versioned<T, history>& v) noexcept { return v.stamps_; }
};

// The awaiter behind `co_await ts::read_last_versions(v)`: the read-guard awaiter's acquire,
// resumed into a `Version_view`. A context that already grants the front is lent - ready at
// once, no turn taken - which is also the await-under-guard rule's stated exemption, an access
// that cannot suspend.
template<typename T>
struct Version_awaiter : Access_awaiter<T, Access::read_only>
{
    Version_awaiter(Scheduler& scheduler, Pipe& pipe, T* front, const T& previous,
                    const Version_stamps& stamps) noexcept
        : Access_awaiter<T, Access::read_only>(scheduler, pipe, front)
        , previous_(previous)
        , stamps_(stamps)
    {
    }

    bool await_ready() noexcept
    {
        const Access_context* ctx = access_load();
        lent_ = ctx != nullptr && ctx->grants(this->obj_, Access::read_only);
        return lent_ || Access_awaiter<T, Access::read_only>::await_ready();
    }

    Version_view<T> await_resume() noexcept
    {
        if (lent_)
            return Version_view<T>(previous_, *this->obj_, stamps_, nullptr);
        this->finish_acquire();
        return Version_view<T>(previous_, *this->obj_, stamps_, &this->pipe_);
    }

    const T& previous_;
    const Version_stamps& stamps_;
    bool lent_ = false;
};

} // namespace detail

// The awaitable form of `Versioned::read_last_versions()`, for a coroutine: takes a read turn
// on the front without blocking a worker, or lends one the coroutine already holds.
//   auto [previous, current, stamps] = co_await ts::read_last_versions(poses);
template<typename T, History history>
    requires (history == History::current_and_previous)
[[nodiscard("co_await it - the view it resumes with is the read grant")]]
detail::Version_awaiter<T> read_last_versions(Versioned<T, history>& versioned)
{
    return detail::Version_awaiter<T>(global_scheduler(), detail::Guarded_access::pipe(versioned.state()),
        detail::Versioned_access::front(versioned), detail::Versioned_access::previous(versioned),
        detail::Versioned_access::stamps(versioned));
}

// The publish step as a graph-node body: declare it with write access on
// `v.state()` - conflict derivation then orders it against every reader, and the
// node's grant is exactly what `publish_into` needs.
//   auto flip = g.add_node("flip", ts::publish_fn(poses), poses.state()).after(sim);
template<typename T, History history = History::current>
struct Publish_fn
{
    Versioned<T, history>* versioned;
    void operator()(T& front) const { versioned->publish_into(front); }
};

template<typename T, History history>
Publish_fn<T, history> publish_fn(Versioned<T, history>& v)
{
    return Publish_fn<T, history>{ &v };
}

// --- out-of-class definitions ----------------------------------------------------------------

inline double Version_stamps::fraction_at(std::chrono::steady_clock::time_point at) const noexcept
{
    if (current_published <= previous_published || at >= current_published)
        return 1.0;
    if (at <= previous_published)
        return 0.0;
    return std::chrono::duration<double>(at - previous_published)
         / std::chrono::duration<double>(current_published - previous_published);
}

template<typename T>
Version_view<T>::Version_view(const T& previous_version, const T& current_version, const Version_stamps& stamps_now,
                              detail::Pipe* held_pipe) noexcept
    : previous(previous_version)
    , current(current_version)
    , stamps(stamps_now)
    , held_pipe_(held_pipe)
{
    if (const Access_context* running = detail::access_load())
        ctx_ = *running;   // extend the running context: a lent view keeps the caller's grant on the front
    if (held_pipe_ != nullptr)
        ctx_.add(&current, Access::read_only, detail::pipe_epoch(*held_pipe_), detail::pipe_rank(*held_pipe_));
    ctx_.add(&previous, Access::read_only);   // no pipe of its own: the front's grant covers it
    prev_ = detail::access_load();
    detail::access_store(&ctx_);
#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
    if (detail::current_coroutine_block() != nullptr)
    {
        detail::guard_depth_add(1);
        counted_ = true;
    }
#endif
}

template<typename T>
Version_view<T>::~Version_view()
{
    detail::access_store(prev_);
#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
    if (counted_)
        detail::guard_depth_add(-1);
#endif
    if (held_pipe_ != nullptr)
        detail::pipe_release(global_scheduler(), *held_pipe_, Access::read_only);
}

template<typename T>
template<std::size_t I>
decltype(auto) Version_view<T>::get() const noexcept
{
    static_assert(I < 3, "a Version_view binds three names: previous, current, stamps");
    if constexpr (I == 0)
        return (previous);
    else if constexpr (I == 1)
        return (current);
    else
        return (stamps);
}

template<typename T, History history>
Version_view<T> Versioned<T, history>::read_last_versions()
    requires (history == History::current_and_previous)
{
    const Access_context* ctx = detail::access_load();
    if (ctx != nullptr && ctx->grants(front_ptr_, Access::read_only))
        return Version_view<T>(this->previous_, *front_ptr_, this->stamps_, nullptr);

    detail::check_version_read_may_block();
    detail::Pipe& pipe = detail::Guarded_access::pipe(front_);
    Signal granted;
    if (!detail::pipe_acquire(global_scheduler(), pipe, Access::read_only, [granted]() mutable { granted.trigger(); }))
        granted.sync();
    return Version_view<T>(this->previous_, *front_ptr_, this->stamps_, &pipe);
}

} // namespace ts

// Tuple protocol for `Version_view`: `auto [previous, current, stamps] = ...` binds references
// to the two versions and the stamps through the view's member `get<I>()`.
template<typename T>
struct std::tuple_size<ts::Version_view<T>> : std::integral_constant<std::size_t, 3>
{
};

template<std::size_t I, typename T>
struct std::tuple_element<I, ts::Version_view<T>>
{
    using type = std::conditional_t<I == 2, const ts::Version_stamps, const T>;
};
