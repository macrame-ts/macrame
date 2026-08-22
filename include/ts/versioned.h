#pragma once

#include "ts/access.h"
#include "ts/fatal.h"
#include "ts/guarded.h"
#include "ts/recorder.h"
#include "ts/task.h"

#include <concepts>
#include <cstddef>
#include <functional>
#include <mutex>
#include <source_location>
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
// `docs/deferred-versioned-state.md`.)
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
template<typename T>
class Versioned
{
    static_assert(std::default_initializable<T>, "Versioned<T>: T must be default-constructible (both replicas)");
    static_assert(std::swappable<T>, "Versioned<T>: publish swaps the replicas' contents");

public:
    // Leading `ts::Named` (a literal, or `ts::Named{}` for the construction site) names the
    // front instance for the DOT dump, the trace and the diagnostics. Required, like
    // `Guarded`'s: the name is what every diagnostic about this object prints.
    template<typename N>
        requires std::same_as<std::remove_cvref_t<N>, Named>
    explicit Versioned(N&& name, Resync policy = Resync::replay)
        : front_(name)
        , policy_(policy)
        , front_ptr_(detail::Guarded_access::instance(front_))
    {
        Signal ready;
        ready.trigger();
        chain_ = ready;          // the "previous publish" of the first publish
#if TS_SAFETY_CHECKS
        last_publish_ = ready;   // the "last" publish's returned gate - done for a fresh instance
#endif
    }

    // With a declared lock rank (`ts::Rank`, access.h), forwarded to the front `Guarded`:
    // required only when the front is dynamically awaited (`co_await ts::read_only(v.state())`)
    // while another grant is held. Without a rank the `access_rank` rule has no order to check
    // against and such an await cannot be satisfied - graph declarations and `read()` need none.
    template<typename N>
        requires std::same_as<std::remove_cvref_t<N>, Named>
    explicit Versioned(N&& name, Rank rank, Resync policy = Resync::replay)
        : front_(name, rank)
        , policy_(policy)
        , front_ptr_(detail::Guarded_access::instance(front_))
    {
        Signal ready;
        ready.trigger();
        chain_ = ready;
#if TS_SAFETY_CHECKS
        last_publish_ = ready;
#endif
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
    // skipped work itself.
    [[nodiscard("await or sync the publish: ~Versioned is fatal while the swap is in flight")]]
    Task<void> publish(Access_options opts = {})
    {
        Signal swapped;        // the returned handle: version visible
        Signal shadow_ready;   // gates the NEXT publish's phase 1

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
        auto phase1 = [this, swapped, shadow_ready, opts]() mutable
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
            }, { .priority = opts.priority });
        };
        detail::core_of(prev)->attach(
            [phase1 = std::move(phase1), priority = opts.priority](void*, bool) mutable
            {
                auto core = detail::make_executable<void>(std::move(phase1), {});
                core->flags.priority = priority;
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

    // Phase 2 work: nanoseconds under the write grant. The scope names both
    // replicas in case T's swap runs instrumented members.
    void swap_replicas(T& front)
    {
        Access_context ctx;
        ctx.add(&front, Access::read_write, detail::pipe_epoch(detail::Guarded_access::pipe(front_)), detail::pipe_rank(detail::Guarded_access::pipe(front_)));
        ctx.add(&shadow_, Access::read_write);   // shadow: no pipe - grant-free by design
        Access_scope scope(ctx);
        using std::swap;
        swap(front, shadow_);
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

// The publish step as a graph-node body: declare it with write access on
// `v.state()` - conflict derivation then orders it against every reader, and the
// node's grant is exactly what `publish_into` needs.
//   auto flip = g.add_node("flip", ts::publish_fn(poses), poses.state()).after(sim);
template<typename T>
struct Publish_fn
{
    Versioned<T>* versioned;
    void operator()(T& front) const { versioned->publish_into(front); }
};

template<typename T>
Publish_fn<T> publish_fn(Versioned<T>& v)
{
    return Publish_fn<T>{ &v };
}

} // namespace ts
