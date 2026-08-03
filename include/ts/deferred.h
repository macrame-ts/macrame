#pragma once

#include "ts/access.h"
#include "ts/fatal.h"
#include "ts/guarded.h"
#include "ts/detail/journal.h"
#include "ts/task.h"

#include <utility>

namespace ts
{

// Deferred mutation of a `Guarded<T>`: producers `stage()` closures into private
// per-recorder storage (no grant on the target, contention-free between recorders),
// and one `commit` applies everything under a single write access. Readers of the
// target between stage and commit see none of the staged writes; after it, all of
// them -- the stable-snapshot contract. Access to the target stays the only
// arbitration mechanism: the commit IS an ordinary write (an `async` job or a graph
// node's declared write access); staging itself needs no synchronization with anyone.
//
// Ordering: FIFO within a recorder (semantic -- build on it); cross-recorder
// order is arbitrary (never build semantics on it) but deterministic given a
// deterministic mint/destroy sequence, so runs are reproducible -- see the
// contract note in journal.h. If a particular recorder's commands must make a
// particular commit, order the producer before the commit (graph edge /
// `after`); commands staged after a commit's cut ride the next one.
//
// The staging machinery (`detail::Journal`, `Recorder`, `Parallel_recorder`) lives
// in journal.h, shared with `Versioned<T>`. See docs/command-buffer-design.md.

// The command buffer: binds to a `Guarded<T>` for its lifetime. `recorder()` mints
// producer handles; `commit()` applies everything staged as ONE write on the target,
// auto-dispatching on grant ownership: called from the task that holds the target's
// write grant (a graph node's declared write, an `async`/`access` write body) it
// applies INLINE under that grant, no second acquisition; called from anywhere else
// it enqueues an ordinary async write (the old `commit_async`). Lifetime contract:
// sync the task an enqueued `commit()` returns before destroying the Deferred (the
// pending write still references this object); violating it is fatal under
// `TS_SAFETY_CHECKS`. Must also outlive any outstanding `Recorder`.
template<typename T>
class Deferred
{
public:
    explicit Deferred(Guarded<T>& target)
        : target_(&target)
    {}

    // Staged-but-uncommitted commands at destruction are lost writes -- the same
    // severity as an undeclared access. Call `discard()` first if dropping them is
    // intended (e.g. teardown mid-frame).
    ~Deferred()
    {
        // Writes on the target are FIFO, so the last-enqueued commit settling means
        // no pending write still references this Deferred; the destructor then has
        // nothing to wait for (unrelated accesses on the target are `~Guarded`'s
        // business, and inline commits finished in-call). An unsettled commit is a
        // contract violation: surface it hard rather than block silently -- a
        // long-parking destructor goes unnoticed and usually marks a bug worth
        // fixing. `last_commit_` is recorded under the pipe mutex atomically with
        // the enqueue (see `pipe_enqueue`), so it can never lag FIFO order; the
        // snapshot takes the same mutex (a commit racing destruction is already UB,
        // the lock keeps the sanctioned quiescent read well-defined).
        detail::Task_ptr last =
            detail::pipe_locked_snapshot(detail::Guarded_access::pipe(*target_), last_commit_);
        if (last != nullptr && !last->ready.load(std::memory_order_acquire))
        {
#if TS_SAFETY_CHECKS
            fatal("Deferred destroyed with a commit still in flight -- sync the "
                  "returned task before destruction");
#endif
            // Shipping safety net: the job dereferences this Deferred when it runs;
            // outwait the target's pending accesses so it cannot run against a
            // destroyed one.
            detail::Guarded_access::pipe(*target_).wait_until_idle();
        }
#if TS_SAFETY_CHECKS
        if (journal_.has_staged())
            fatal("Deferred destroyed with staged uncommitted commands (lost writes); commit or discard() first");
#endif
    }

    Deferred(const Deferred&) = delete;
    Deferred& operator=(const Deferred&) = delete;

    // Mint a producer handle. A destroyed recorder's slot is recycled (free-list),
    // so live slots are bounded by peak concurrent recorders. Cross-recorder
    // order is arbitrary-but-reproducible -- see the ordering contract in
    // journal.h.
    Recorder<T> recorder()
    {
        return Recorder<T>(journal_, journal_.add_slot());
    }

    // Mint a per-worker handle for parallel staging (see `Parallel_recorder`).
    // Same lifetime rule: mint once, reuse.
    Parallel_recorder<T> parallel_recorder()
    {
        return Parallel_recorder<T>(journal_, global_scheduler());
    }

    // Apply everything staged so far as ONE write on the bound object, auto-dispatching
    // on grant ownership (the object is implicit, so there is no way to commit into the
    // wrong instance):
    //
    //  1. The calling task holds the target's write grant (`Pipe::writer_owner` is this
    //     task -- a graph node's declared write, an `async`/`access` write body): apply
    //     INLINE under that grant, cut now. Returns a pre-settled task; NOTE it provides
    //     no happens-before edge (it settled before the apply) -- observers of the data
    //     order through the object's pipe, which orders.
    //  2. Anyone else: enqueue as an ordinary async write on the target -- one access
    //     acquisition amortized over the whole batch; the cut happens when the write
    //     RUNS. Returns the write's completion; sync it before destroying the Deferred.
    //
    // The enqueue records the write into `last_commit_` atomically (under the pipe
    // mutex), so concurrent fire-and-forget commits keep the destructor's settled check
    // exact -- no external lock. Contract: from NESTED sub-work running under a parent's
    // inherited write grant, call `commit()` in the grant-holding task instead -- the
    // nested task is not the holder, and the enqueued write would queue behind the very
    // grant it waits out (fatal under `TS_SAFETY_CHECKS`, a silent deadlock-on-sync
    // otherwise).
    Task<void> commit(Access_options opts = {})
    {
        detail::Pipe& pipe = detail::Guarded_access::pipe(*target_);
        detail::Task_control_block* owner = pipe.writer_owner.load(std::memory_order_acquire);
        if (owner != nullptr && owner == detail::current_task.get())
        {
            if (opts.token.is_cancel_requested())
                return detail::task_from_core<void>(detail::cancelled_void_core());   // batch retained
            apply_under_grant();
            return detail::task_from_core<void>(detail::settled_void_core());
        }
#if TS_SAFETY_CHECKS
        // Nested sub-work under an inherited write grant: not the holder (`writer_owner`
        // is the parent), and enqueueing would deadlock behind the parent's own hold the
        // moment anyone syncs it. Surface the misuse at the call.
        if (detail::current_access != nullptr
            && detail::current_access->holds_write_epoch(detail::pipe_epoch(pipe)))
            fatal("Deferred::commit() from nested sub-work under an inherited write grant -- "
                  "commit from the task holding the grant (the node/async body) instead");
#endif
        return detail::Guarded_access::commit_write(
            *target_, [this](T&) { apply_under_grant(); }, opts, &last_commit_);
    }

    // Drop everything staged so far, explicitly. The escape hatch for teardown.
    void discard()
    {
        journal_.cut();
    }

private:
    // The apply half shared by both `commit()` arms: requires (and checks) a write grant
    // on the bound object; cuts now and applies the batch.
    void apply_under_grant()
    {
        T* t = detail::Guarded_access::instance(*target_);
#if TS_SAFETY_CHECKS
        access_check(t);   // requires a read_write grant on the bound object
#endif
        auto batch = journal_.cut();
        for (auto& cmd : batch)
            cmd(*t);
    }

    Guarded<T>* target_;
    detail::Journal<T> journal_;
    // The most recent ENQUEUED commit's block; the destructor's in-flight check (null
    // until a commit enqueues; inline commits finish in-call and never record). Written
    // under the target pipe's mutex atomically with the enqueue (`pipe_enqueue`'s
    // `record`), so FIFO order and record order cannot diverge -- the property the old
    // `commit_mutex_` existed to enforce externally. Unconditional (not safety-only):
    // it gates the shipping destructor's wait-skip.
    detail::Task_ptr last_commit_;
};

} // namespace ts
