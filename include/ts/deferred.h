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
// them -- the stable-snapshot contract. The pipe stays the only arbitration
// mechanism: the commit IS an ordinary write (an `async` job or a graph node's
// declared write access); staging itself needs no synchronization with anyone.
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
// producer handles; `commit_async()` applies everything as one pipe write job;
// `commit(T&)` applies under an access grant the caller already holds (a graph
// node's declared write, or inside a `target.async` write body) -- no second pipe
// round-trip. Must outlive any pending `commit_async` (the destructor waits out
// the target's pipe) and any outstanding `Recorder`.
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
        // Let any queued commit_async run before judging leftovers.
        detail::Guarded_access::pipe(*target_).wait_until_idle();
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

    // Apply everything staged so far to `target`, which the caller must already
    // hold write access to (checked). Cut happens now: commands staged after this
    // point ride the next commit.
    void commit(T& target)
    {
#if TS_SAFETY_CHECKS
        access_check(&target);   // non-const overload: requires a read_write grant
#endif
        auto batch = journal_.cut();
        for (auto& cmd : batch)
            cmd(target);
    }

    // Apply as an ordinary pipe write job: one acquisition amortized over the whole
    // batch. The cut happens when the job RUNS (pipe FIFO position), so it captures
    // everything staged before the write actually happens. Returns the completion.
    Task<void> commit_async(Access_options opts = {})
    {
        return target_->async([this](T& t) { commit(t); }, opts);
    }

    // Drop everything staged so far, explicitly. The escape hatch for teardown.
    void discard()
    {
        journal_.cut();
    }

private:
    Guarded<T>* target_;
    detail::Journal<T> journal_;
};

} // namespace ts
