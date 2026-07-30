#pragma once

#include "ts/access.h"
#include "ts/fatal.h"
#include "ts/guarded.h"
#include "ts/detail/journal.h"
#include "ts/task.h"

#include <mutex>
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
// producer handles; `commit_async()` applies everything as one async write on the
// target; `commit()` applies to the bound object under an access grant the caller
// already holds (a graph node's declared write, or inside a `target.async` write
// body) -- without a second access acquisition. Lifetime contract: sync the task
// `commit_async` returns before destroying the Deferred (the pending write still
// references this object); violating
// it is fatal under `TS_SAFETY_CHECKS`. Must also outlive any outstanding `Recorder`.
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
        // Writes on the target are FIFO, so the last-submitted commit settling means
        // no pending write still references this Deferred; the destructor then has
        // nothing to wait for (unrelated accesses on the target are `~Guarded`'s
        // business). An unsettled commit is a contract violation: surface it hard
        // rather than block silently -- a long-parking destructor goes unnoticed
        // and usually marks a bug worth fixing.
        // Snapshot under the commit lock (a commit_async racing destruction is
        // already UB; the lock still keeps the read well-defined and ordered).
        Task<void> last;
        bool issued;
        {
            std::lock_guard lock(commit_mutex_);
            last = last_commit_;
            issued = commit_issued_;
        }
        if (issued && !last.is_done())
        {
#if TS_SAFETY_CHECKS
            fatal("Deferred destroyed with a commit_async still in flight -- sync the "
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

    // Apply everything staged so far to the bound object, which the caller must
    // already hold write access to (checked). The object is implicit (this Deferred
    // binds one `Guarded<T>`), so there is no way to commit into the wrong instance.
    // Cut happens now: commands staged after this point ride the next commit.
    void commit()
    {
        T* t = detail::Guarded_access::instance(*target_);
#if TS_SAFETY_CHECKS
        access_check(t);   // requires a read_write grant on the bound object
#endif
        auto batch = journal_.cut();
        for (auto& cmd : batch)
            cmd(*t);
    }

    // Apply as an ordinary async write on the target: one access acquisition amortized
    // over the whole batch. The cut happens when the write RUNS (its turn in the
    // target's write order), so it captures everything staged before the write actually
    // happens. Returns the completion; sync it before destroying the Deferred (see the
    // destructor). Callable concurrently (producers may fire-and-forget); the lock spans
    // submit + store so `last_commit_` is the last commit in submission order -- store
    // order alone would let an earlier write's handle overwrite a later one's, and the
    // destructor's settled check would then miss a still-pending write. Uncontended in
    // single-committer use; the submit inside the lock cannot re-enter (a commit body
    // never calls `commit_async`).
    Task<void> commit_async(Access_options opts = {})
    {
        std::lock_guard lock(commit_mutex_);
        last_commit_ = target_->async([this](T&) { commit(); }, opts);
        commit_issued_ = true;
        return last_commit_;
    }

    // Drop everything staged so far, explicitly. The escape hatch for teardown.
    void discard()
    {
        journal_.cut();
    }

private:
    Guarded<T>* target_;
    detail::Journal<T> journal_;
    // The most recent `commit_async`'s completion; the destructor's in-flight
    // check. FIFO makes one handle cover all commits. Unconditional (not
    // safety-only): it gates the shipping destructor's wait-skip. `Task` has no
    // null test, hence the flag.
    Task<void> last_commit_;
    bool commit_issued_ = false;
    // Orders concurrent commit_async calls (enqueue + handle store as one step).
    // Interim: the pipes-as-edges rebase (TODO 1.14) exposes the pipe's own tail
    // as the last-write handle, deleting this mutex -- see the 1.14 addendum.
    std::mutex commit_mutex_;
};

} // namespace ts
