#pragma once

// The per-object reader/writer serializer - the `Pipe` (docs/pipe-rebase.md §0). Every
// `Guarded`/`Versioned` owns exactly one, and it is what orders conflicting accesses to that
// object: concurrent readers run at once, one exclusive writer runs alone, all in FIFO
// admission order. Different objects have independent pipes and run in parallel. The pipe is
// non-blocking - no caller ever waits in it; admission is completion-driven, and an admitted
// entry's turn fires `release()` on its owner block (the pipe is a prerequisite source for the
// task machinery, not a dispatcher). Queueing allocates nothing: the queue is the tasks' own
// embedded `Pipe_link`s threaded intrusively.
//
// This header carries the `Pipe` struct, its cascade entry points (`pipe_enter_first`,
// `advance_pipe_links`, `pipe_acquire`/`pipe_release`, `pipe_try_inline`), the waits-for cycle
// detector hooks, and the pipe-task block builders (`Piped_executable`, `make_piped_executable`,
// `bind_pipe_link`). The public `Guarded<T>` and the free access verbs live in ts/guarded.h; the
// compile-time access-mode deduction in ts/detail/access_deduction.h. Serializer internals and
// the evolved cascade: docs/pipe-rebase.md §0; the per-node turn mechanism: docs/task-internals.md §10.

#include "ts/access.h"
#include "ts/detail/pipe_link.h"
#include "ts/named.h"
#include "ts/scheduler.h"
#include "ts/task.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

namespace ts
{

namespace detail
{

// A per-object reader/writer pipe (the evolved mutex pipe, docs/pipe-rebase.md §0.2).
// Entries are admitted in FIFO order; consecutive readers run concurrently, a writer runs
// alone (no readers, no other writer). Different objects have independent pipes and run in
// parallel. Non-blocking: callers never wait; admission is completion-driven, and an
// admitted entry's turn fires `release()` on its owner - the pipe is a prerequisite
// source for the block machinery, not a dispatcher. The queue is the tasks' own embedded
// `Pipe_link`s threaded intrusively, so queueing allocates nothing.
struct Pipe
{
    std::mutex mutex;
    std::condition_variable idle;
    Pipe_link* queue_head = nullptr;   // waiting (not yet admitted) entries, FIFO
    Pipe_link* queue_tail = nullptr;
    int active_readers = 0;
    bool writer_active = false;
#if TS_SAFETY_CHECKS
    // Grant-window epoch for the harness's stale-inherited-grant check (see
    // `Access_context`). Seqlock-style parity: bumped at every write-grant acquire and
    // release (always under `mutex`), so it is odd while a writer holds and even during
    // reader eras. Reader
    // acquires/releases do not bump - a read grant goes stale exactly when a writer
    // acquires after its capture, which is the actual safety condition. Fully compiled
    // out with the harness (the gating convention: safety-only state carries no
    // shipping cost; mixed-config TUs are an ODR violation caught by the link-time
    // tripwire in access.h).
    std::atomic<std::uint64_t> write_epoch{ 0 };
    // Count of compiled `Static_task_graph`s whose `distinct_pipes_` reference this pipe
    // (`compile()` +1; graph destruction, recompile, and move-assign-overwrite -1).
    // `~Guarded` fatals while it is nonzero: destroying the object would leave the graph
    // holding dangling pipe/instance pointers for its next run (the Taskflow-#82 class of
    // lifetime misuse, caught at the cause instead of crashing far from it).
    std::atomic<int> graph_refs{ 0 };
#endif
    // Identity of the owning `Guarded`/`Versioned` (`ts::Named`): a literal or the
    // construction site, referenced not copied. Consumed by the graph's DOT dump (edge
    // tooltips), the trace, and the access diagnostics; kept in all builds (three words -
    // a handful of objects, and the consumers are shipping-capable).
    Named debug_name{ nullptr };
#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
    // The object's declared `ts::Rank`; 0 = unranked (the strict default - see `ts::Rank`).
    unsigned rank = 0;
#endif

    // The block currently holding this pipe's write grant, null outside a write window
    // (docs/pipe-rebase.md §0.2). Always-on: behavior keys off it (`Deferred::commit`
    // applies inline when the caller is the holder), so it cannot live behind
    // `TS_SAFETY_CHECKS`. Written under `mutex` at write admission/release; read lock-free
    // by the ownership check. Identity only - never dereferenced.
    std::atomic<Task_control_block*> writer_owner{ nullptr };

    // Blocks until the pipe is fully drained and nothing is in flight. Teardown-only
    // (`~Guarded`). Safe against the completing job that signals it: the notify is done
    // under `mutex` (`release_and_redispatch`), so this waiter cannot rewake and destroy
    // the pipe until it re-acquires `mutex` after the signaler's notify returns - no
    // UE-`FPipe`-class refcounted drain event needed (that race is a lock-free-notify
    // artifact). See `release_and_redispatch`.
    void wait_until_idle()
    {
        std::unique_lock lock(mutex);
        idle.wait(lock, [this]
        {
            return queue_head == nullptr && active_readers == 0 && !writer_active;
        });
    }
};

// Grant-epoch source for `Access_context::add`: the pipe's `write_epoch` under
// `TS_SAFETY_CHECKS`, null with the harness compiled out (the three-argument `add`
// ignores a null source). Lets the capture sites - which compile in both configs -
// name the fully-gated field through one spelling.
inline const std::atomic<std::uint64_t>* pipe_epoch([[maybe_unused]] const Pipe& pipe) noexcept
{
#if TS_SAFETY_CHECKS
    return &pipe.write_epoch;
#else
    return nullptr;
#endif
}

// The pipe's declared `ts::Rank`, or 0 when the rank rule is compiled out. Same shape as
// `pipe_epoch`: one spelling for the capture sites, which compile in every config.
inline unsigned pipe_rank([[maybe_unused]] const Pipe& pipe) noexcept
{
#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
    return pipe.rank;
#else
    return 0;
#endif
}

// Enter a pipe task's first link into its pipe's queue (starting the sequential canonical
// cascade for a multi-object task - each admitted turn enters the owner's next link and
// fires `release(owner)`; the last turn's release dispatches through the standard
// `dispatch_ready`). The block must have its links bound (`bind_pipe_link`) and
// `num_locks` seeded with `pipe_count`. `record`, when non-null, receives the block while
// still under the first pipe's mutex - the atomic enqueue-and-record `Deferred::commit`
// needs so its recorded handle can never lag the pipe's FIFO order (the race the deleted
// `commit_mutex_` used to close).
void pipe_enter_first(Task_control_block* blk, Task_ptr* record);   // default arg on the task.h declaration

// Advance (release) every entered link of a settled pipe task. The settle-must-advance-
// links contract: every pipe-task creation site must route its settle through this -
// `make_piped_executable` installs it as `on_complete`; the graph's `graph_node_completed`
// calls it first. A missed call wedges the affected pipes.
void advance_pipe_links(Task_control_block* blk);

// `on_complete` hook form of the above, for tasks whose settle needs nothing else.
void pipe_links_on_complete(Task_control_block* blk);

// Acquire a pipe as a held grant in `mode` (the coroutine guards' primitive - everything
// else rides links on its own block): a `read_only` hold joins concurrent readers, a
// `read_write` hold is exclusive, released only by `pipe_release`. Returns true if
// acquired in-call (no callback fires); false if deferred, in which case `on_acquired`
// runs once the pipe drains to it (FIFO), scheduled on `scheduler`. `owner` is the
// grant-holder block a write hold publishes through `Pipe::writer_owner` (null when no
// block identity exists). A deferred hold allocates one small queue node - the one
// allocating pipe path.
bool pipe_acquire(Scheduler& scheduler, Pipe& pipe, Access mode, std::move_only_function<void()> on_acquired,
                  Task_control_block* owner = nullptr);

// Release a hold taken by `pipe_acquire` in `mode`; admits queued entries.
void pipe_release(Scheduler& scheduler, Pipe& pipe, Access mode);

#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)
// Waits-for cycle detector (docs/coroutine-first.md §2). At a genuine suspension on a
// pipe - a deferred coroutine guard acquire, or awaiting a pipe-job task - the awaiters
// record one edge per held grant: {pipe the suspending context holds -> pipe it awaits}.
// Held pipes come from `held`'s epoch sources; a cycle among the recorded edges is the
// suspended-ABBA deadlock (no thread parks - both frames are suspended and every worker
// is free - so nothing would ever diagnose it at runtime), fatal at the moment the
// closing edge is inserted, naming both tasks and both objects. `ticket` identifies this
// suspension (the awaiter's address); `waiter` is the suspending task's block for the
// diagnostic (may be null off-task). Returns whether any edge was recorded, so the resume
// path knows to `circular_wait_clear`. Insertion and the cycle check share one registry lock,
// so a true deadlock is always caught by whichever awaiter inserts last. A granted-but-
// not-yet-cleared edge (the grant fires between record and resume) can in principle close
// a spurious cycle in that window; the window is a few instructions on the resume path and
// requires a reader-share interleaving to matter - accepted for a safety harness.
bool circular_wait_record(const Access_context* held, const void* ticket, const Task_control_block* waiter,
                      Pipe* const* awaited, int count);
void circular_wait_clear(const void* ticket) noexcept;
#endif

// An executable pipe task: the `Executable` wrapper plus its embedded per-pipe links -
// one allocation for block + result + body + links (docs/pipe-rebase.md §0.2). `exec` is
// the first member and `Executable::core` its first, so the intrusive handle aliases the
// whole wrapper as usual.
template<typename Body, typename R, std::size_t N>
struct Piped_executable
{
    Executable<Body, R> exec;
    Pipe_link links[N];

    explicit Piped_executable(Body b)
        : exec(std::move(b))
    {}
};

// Build a pipe task with capacity for `N` links (bind them with `bind_pipe_link`, then set
// `num_locks` to the bound count and enter). Installs `pipe_links_on_complete` - the
// settle-must-advance-links contract's factory half (the graph's node blocks are the other
// creation site; see `graph_node_completed`).
template<typename R, std::size_t N, typename Body>
Task_ptr make_piped_executable(Body&& body, Cancellation_token token)
{
    using Exec = Executable<std::decay_t<Body>, R>;
    using Wrapper = Piped_executable<std::decay_t<Body>, R, N>;
    auto* w = new Wrapper(std::forward<Body>(body));
    Task_control_block& core = w->exec.core;
    core.destroy = [](Task_control_block* c) { delete reinterpret_cast<Wrapper*>(c); };
    core.execute = &Exec::run;
    core.on_complete = &pipe_links_on_complete;
    core.token = std::move(token);
    core.pipe_links = w->links;
    return Task_ptr(&core);   // refcount 0 -> 1, owns the wrapper
}

// Bind the block's next link to (pipe, mode). Canonical order is the caller's contract:
// links must be bound in ascending pipe-address order (single-object trivially; the
// multi-object builder sorts; the graph's `pipe_indices` are ascending over the
// address-sorted `distinct_pipes_`).
inline void bind_pipe_link(Task_control_block* core, std::uint8_t index, Pipe& pipe, Access mode)
{
    Pipe_link& l = core->pipe_links[index];
    l.owner = core;
    l.pipe = &pipe;
    l.index = index;
    l.mode = mode;
    core->pipe_count = static_cast<std::uint8_t>(index + 1);
}

// Try to run a job inline on the calling thread instead of enqueuing it (the `access` verb's
// fast path). Admissible only when the pipe is immediately free for this mode - no queued jobs
// (FIFO preserved) and the reader/writer rules allow: `read_only`
// joins as a concurrent reader, `read_write` as an exclusive writer. On success runs the
// block's body synchronously (the caller blocks for its duration), then releases,
// re-dispatches the pipe, and returns true. On failure returns false and the caller
// enqueues the same block. Caller-blocking + a nested access scope - see `Guarded::async`.
bool pipe_try_inline(Scheduler& scheduler, Pipe& pipe, Access mode, const Task_ptr& block);

} // namespace detail
} // namespace ts
