#pragma once

#include "access.h"
#include "fatal.h"
#include "guarded.h"
#include "journal.h"
#include "task.h"

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace ts
{

// How `publish()` brings the shadow replica back in sync after the swap (both
// replicas must be identical at the start of every publish -- the invariant that
// makes staged deltas equivalent to full state).
enum class Resync
{
    // Re-apply the same batch to the new shadow (the old front). Both applications
    // see bit-identical pre-states, so deterministic commands produce bit-identical
    // replicas -- cost proportional to the delta, not to sizeof(T). Requires
    // commands to be deterministic (capture RNG/time/etc. results at stage time,
    // never read them inside the closure). The default.
    replay,

    // Copy front over shadow after the swap (user fn via `set_copy`, or T's copy
    // assignment). For nondeterministic commands or cheap-to-copy T. Cost
    // proportional to the state.
    copy,

    // No resync: the user contract is that every version's staged writes fully
    // overwrite the state (per-frame event lists, cleared and refilled). Partial
    // writes under this policy read stale data -- that is the user's assertion.
    overwrite,
};

// Versioned state: readers always see a stable published version while the next
// one is being staged. Two replicas of `T` behind one `Guarded` front -- readers
// take ordinary read access on `state()`; writes are `stage()`d grant-free into a
// journal (see journal.h) and become visible atomically at `publish()`. The
// front's address never changes (the swap exchanges the replicas' CONTENTS), so
// graph declarations, the pipe, and the harness all work unmodified.
//
// A publish runs in three phases, and only the middle one holds the write grant:
//   1. cut + apply the batch to the shadow -- grant-free (nobody can observe the
//      shadow); readers of the current version run concurrently.
//   2. swap the replicas' contents -- the ONLY work under the write grant (a
//      move-swap; pointer exchanges for container-backed T).
//   3. resync the shadow -- a READ job on the front's pipe: it overlaps every
//      reader of the new version, and pipe FIFO holds the next writer (a later
//      publish's swap, or a graph flip node's acquire) behind it. The pipe is the
//      shadow-ownership chain; consecutive publishes additionally chain phase 1
//      after the previous resync internally.
// (The graph-node form `publish_into` runs phases 1-2 under the node's grant --
// edges order readers around the node anyway -- and still defers phase 3 to the
// read job, which the pipe admits the moment the node releases.)
//
// The contract in one line: no read-your-writes -- a version's outputs arrive as
// the NEXT version. Readers wanting the fresh version order after the publish
// (graph edge / `.after(publish_task)`).
//
// One publisher at a time: dynamic `publish()` calls may overlap each other (they
// chain internally), but do not run dynamic publishes concurrently with a graph
// whose flip node publishes the same `Versioned` -- same discipline as the
// graph's one-run-at-a-time rule.
template<typename T>
class Versioned
{
    static_assert(std::default_initializable<T>, "Versioned<T>: T must be default-constructible (both replicas)");
    static_assert(std::swappable<T>, "Versioned<T>: publish swaps the replicas' contents");

public:
    explicit Versioned(Resync policy = Resync::replay)
        : policy_(policy)
        , front_ptr_(detail::Guarded_access::instance(front_))
    {
        Signal ready;
        ready.trigger();
        chain_ = ready;   // the "previous publish" of the first publish
    }

    // Same lost-writes policy as `Deferred`: staged-but-unpublished commands at
    // destruction are fatal under TS_SAFETY_CHECKS; `discard()` is the escape.
    ~Versioned()
    {
        // Drain the publish chain (phase 1/2 tasks are not pipe jobs), then the
        // pipe (reads, swaps, resyncs), then judge leftovers.
        Task<void> chain;
        {
            std::lock_guard lock(seq_mutex_);
            chain = chain_;
        }
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
    // nondeterministic, so commands should be per-key single or commutative --
    // the batch order is still fixed at the cut, so `replay` resync stays exact.
    Parallel_recorder<T> parallel_recorder()
    {
        return Parallel_recorder<T>(journal_, default_scheduler());
    }

    // Read the current published version -- an ordinary read job on the front's
    // pipe (concurrent readers overlap; a queued publish orders around it, FIFO).
    template<typename Fn>
        requires detail::Async_accessor<Fn, const T&>
    auto read(Fn&& fn, Task_options opts = {}) const
    {
        return std::as_const(front_).async(std::forward<Fn>(fn), opts);
    }

    // The front's `Guarded` -- for static-graph declarations. Declare READ access
    // only; the one sanctioned writer is the publish node (`publish_body`).
    // Writing it directly bypasses versioning and breaks the replica invariant.
    Guarded<T>& state() { return front_; }
    const Guarded<T>& state() const { return front_; }

    // Publish everything staged so far as one atomic version step. The returned
    // task completes at the swap (phase 2) -- order fresh readers after it; the
    // resync continues past it, invisibly. An empty journal is a no-op. A
    // cancelled `opts.token` skips the step (commands stay staged for the next
    // publish); the returned task still COMPLETES -- it is a phase gate, not the
    // skipped work itself.
    Task<void> publish(Task_options opts = {})
    {
        Signal swapped;        // the returned handle: version visible
        Signal shadow_ready;   // gates the NEXT publish's phase 1

        Task<void> prev;
        {
            std::lock_guard lock(seq_mutex_);
            prev = std::exchange(chain_, shadow_ready);
        }

        // Phase 1 runs once the previous publish's resync is done (shadow free).
        prev.then([this, swapped, shadow_ready, opts]() mutable
        {
            if (opts.token.is_cancel_requested())
            {
                swapped.trigger();
                shadow_ready.trigger();
                return;
            }
            auto batch = std::make_shared<Batch>(journal_.cut());
            if (batch->empty())
            {
                swapped.trigger();   // readers keep the current version, which equals the would-be next
                shadow_ready.trigger();
                return;
            }
            apply_to_shadow(*batch);

            // Phase 2: the only write on the pipe -- swap and get out.
            front_.async([this, batch, swapped, shadow_ready](T& front) mutable
            {
                swap_replicas(front);
                swapped.trigger();
                start_resync(std::move(batch), std::move(shadow_ready));
            }, { .priority = opts.priority });
        }, { .priority = opts.priority });

        return swapped;
    }

    // Publish under a write grant the caller already holds -- the graph-node form
    // (see `publish_body`). `front` must be this Versioned's front instance.
    // Phases 1-2 run inline (the node holds the grant regardless; edges already
    // order readers around it); phase 3 is deferred to the pipe read job, admitted
    // the moment the node releases its objects.
    void publish_into(T& front)
    {
#if TS_SAFETY_CHECKS
        if (&front != front_ptr_)
            fatal("Versioned::publish_into: not this Versioned's front instance");
        access_check(&front);
#endif
        auto batch = std::make_shared<Batch>(journal_.cut());
        if (batch->empty())
            return;

        apply_to_shadow(*batch);
        swap_replicas(front);

        Signal shadow_ready;
        {
            std::lock_guard lock(seq_mutex_);
            chain_ = shadow_ready;   // single-publisher discipline: the previous chain is complete
        }
        start_resync(std::move(batch), std::move(shadow_ready));
    }

    // Custom copy for `Resync::copy` (dst = shadow, src = front). Optional when T
    // is copy-assignable.
    void set_copy(std::function<void(T& dst, const T& src)> copy)
    {
        copy_ = std::move(copy);
    }

    // Opt-in replay verification (TS_SAFETY_CHECKS): after every replay resync,
    // hash both replicas and fatal on mismatch. Bitwise equality is the right
    // check -- replay-twice has no FP drift (same commands, same order, same
    // pre-state, same binary). Partial hashes (hot arrays only) are fine.
    void set_divergence_check(std::function<std::size_t(const T&)> hash)
    {
        hash_ = std::move(hash);
    }

    // Drop everything staged so far (teardown escape hatch).
    void discard()
    {
        journal_.cut();
    }

private:
    using Batch = std::vector<typename detail::Journal<T>::Command>;

    // Phase 1 work: the shadow is unobservable, so this needs no grant on the
    // front -- readers of the current version run concurrently.
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
        ctx.add(&front, Access::read_write);
        ctx.add(&shadow_, Access::read_write);
        Access_scope scope(ctx);
        using std::swap;
        swap(front, shadow_);
    }

    // Phase 3: bring the new shadow (old front contents) to the new version, as a
    // READ job on the front's pipe -- overlapping readers, FIFO-ordered before the
    // next writer. `overwrite` needs no work (and no job).
    void start_resync(std::shared_ptr<Batch> batch, Signal shadow_ready)
    {
        if (policy_ == Resync::overwrite)
        {
            shadow_ready.trigger();
            return;
        }
        std::as_const(front_).async([this, batch = std::move(batch), shadow_ready](const T& front) mutable
        {
            {
                Access_context ctx;
                ctx.add(&shadow_, Access::read_write);
                ctx.add(&front, Access::read_only);
                Access_scope scope(ctx);
                if (policy_ == Resync::replay)
                {
                    // Both replicas held version N-1 when the batch was cut, so
                    // this second application sees the same pre-state as the first
                    // -- deterministic commands land both replicas at bit-identical
                    // version N.
                    for (auto& cmd : *batch)
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
                    fatal("Versioned<T>: replica divergence after replay resync -- a staged command is nondeterministic");
#endif
            }
            shadow_ready.trigger();
        });
    }

    Guarded<T> front_;                // readers' pipe + the published replica
    T shadow_{};                      // the replica the next version is built in (value-init: must equal the front's initial state)
    detail::Journal<T> journal_;
    Resync policy_;
    T* front_ptr_;
    std::mutex seq_mutex_;            // guards `chain_` handoff between publishes
    Task<void> chain_;                // the latest publish's shadow_ready -- the next phase 1 gates on it
    std::function<void(T&, const T&)> copy_;
    std::function<std::size_t(const T&)> hash_;
};

// The publish step as a graph-node body: declare it with write access on
// `v.state()` -- conflict derivation then orders it against every reader, and the
// node's grant is exactly what `publish_into` needs.
//   auto flip = g.add_node(ts::publish_body(poses), poses.state()).after(sim);
template<typename T>
struct Publish_body
{
    Versioned<T>* versioned;
    void operator()(T& front) const { versioned->publish_into(front); }
};

template<typename T>
Publish_body<T> publish_body(Versioned<T>& v)
{
    return Publish_body<T>{ &v };
}

} // namespace ts
