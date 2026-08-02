#include "ts/guarded.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ts
{

namespace
{
// The one process-wide scheduler, reconfigurable via `configure_scheduler`. A `unique_ptr`
// in a namespace static so program exit destroys it (joins its workers) -- clean shutdown,
// as the old Meyers static gave. `g_fast` is a lock-free read cache for the hot path (every
// `global_scheduler()` from an external thread); the mutex guards lazy init + the
// teardown/recreate swap. Reconfigure racing concurrent use is undefined (documented) -- the
// `g_fast` reset before teardown is belt-and-suspenders, not a use-vs-reconfigure guarantee.
std::mutex g_sched_mutex;
std::unique_ptr<Scheduler> g_scheduler;
Scheduler_config g_config;
std::atomic<Scheduler*> g_fast{ nullptr };
}

Scheduler& global_scheduler()
{
    if (Scheduler* s = g_fast.load(std::memory_order_acquire))
        return *s;
    std::lock_guard lock(g_sched_mutex);
    if (!g_scheduler)
    {
        g_scheduler = std::make_unique<Scheduler>(g_config);
        g_fast.store(g_scheduler.get(), std::memory_order_release);
    }
    return *g_scheduler;
}

Scheduler_config current_scheduler_config()
{
    std::lock_guard lock(g_sched_mutex);
    return g_config;
}

void configure_scheduler(Scheduler_config config)
{
    std::lock_guard lock(g_sched_mutex);
    g_fast.store(nullptr, std::memory_order_release);
    g_scheduler.reset();   // dtor: quit + join workers, drain queued tasks
    g_config = config;
    g_scheduler = std::make_unique<Scheduler>(config);
    g_fast.store(g_scheduler.get(), std::memory_order_release);
}

namespace detail
{

void submit_closure(Scheduler& scheduler, std::move_only_function<void()> closure, Priority priority)
{
    auto* held = new std::move_only_function<void()>(std::move(closure));
    scheduler.submit(
        [](void* data)
        {
            auto* fn = static_cast<std::move_only_function<void()>*>(data);
            (*fn)();
            delete fn;
        },
        held, priority);
}

// Trampoline for a queued block dispatch: the block travels as the entry's `data_` (its ref
// adopted from the queue), the reuse generation via the block's `dispatch_arg`. No heap
// closure -- the block IS the payload -- so a bare task dispatch allocates nothing beyond its
// own block (killing the per-dispatch `submit_closure` alloc that hit every queued task).
static void run_block_dispatch(void* data)
{
    Task_ptr block(static_cast<Task_control_block*>(data), Adopt_ref{});   // adopt the queued ref
    std::uint64_t gen = block->dispatch_arg.load(std::memory_order_acquire);
    TS_FORENSIC(block.get(), E_pop, gen, 0);
    TS_FORENSIC_PATH(1);
    if (block->execute)
        block->execute(block, gen);              // claims `gen` internally (dedup + stale-skip)
    else if (block->claim(gen))                  // bodyless: claim so a stale/duplicate no-ops
        block->complete();
    TS_FORENSIC_PATH(0);
}   // `block` decrements here -> releases the ref the queue held

// A block whose prerequisites are all met: schedule it to run (its body, or, if
// bodyless, just complete). Bridges task.h's lock-counter to the scheduler.
//
// `gen` was captured by the RELEASER at/before the `num_locks` decrement that hit zero (see
// `release`), so it names the run that actually became ready -- it is never re-read here.
// Re-reading `generation()` at this point was the premature-dispatch TOCTOU: a releaser
// preempted between its decrement and this call can wake AFTER the run it released was
// retracted, consumed, and re-armed -- the re-read stamped the NEXT generation, whose claim
// then succeeded while that round's prerequisites were still unmet (captured live by
// tsan/reuse_hunt.sh: 112/400 iterations on a plain 2-core-pinned build).
void submit_ready(Task_ptr block, std::uint64_t gen)
{
    Priority priority = block->flags.priority;
    // Publish `gen` with a MONOTONIC-MAX CAS, not a plain store: the same delayed releaser
    // must not regress `dispatch_arg` below a newer round's publish (that would orphan the
    // newer round's dispatch -- both entries would fail `claim`, and a non-retracting waiter
    // would hang). Generations only increase, so max is sound; the loop is per-dispatch
    // (cold) and virtually always uncontended. Invariant: every value in `dispatch_arg` is a
    // generation whose run genuinely released to zero, so a popped entry reads either its own
    // gen (claim fails after a reset) or a newer READY one (safe early run; claim de-dups).
    std::uint64_t cur = block->dispatch_arg.load(std::memory_order_relaxed);
    while (cur < gen && !block->dispatch_arg.compare_exchange_weak(
               cur, gen, std::memory_order_release, std::memory_order_relaxed))
    {
    }
    TS_FORENSIC(block.get(), E_submit, gen, 0);
    // Hand the block's ref to the queue (release, no dec); the trampoline adopts it back.
    global_scheduler().submit(&run_block_dispatch, block.release(), priority);
}

#if !TS_PIPE_TAIL   // ===== current mutex-guarded reader/writer deque pipe =====

namespace
{

// Jobs admitted by one `dispatch` pass, collected under the pipe mutex and SUBMITTED AFTER
// it is released (see `dispatch`). Small inline buffer: a pass admits one writer or a short
// run of readers.
using Admitted = std::vector<Job>;

void dispatch(Pipe& pipe, Admitted& admitted);
void submit_admitted(Scheduler& scheduler, Pipe& pipe, Admitted& admitted);

// Release the pipe in `mode` and admit whatever the release unblocks; notify a
// `wait_until_idle` waiter if the pipe drained. The tail shared by every way a pipe access
// ends (queued job body returning, inline body returning, `pipe_release`).
void release_and_redispatch(Scheduler& scheduler, Pipe& pipe, Access mode)
{
    Admitted admitted;
    {
        std::scoped_lock lock(pipe.mutex);
        if (mode == Access::read_only)
        {
            --pipe.active_readers;
        }
        else
        {
            pipe.writer_active = false;
#if TS_SAFETY_CHECKS
            pipe.write_epoch.fetch_add(1, std::memory_order_relaxed);   // write window closes
#endif
        }

        dispatch(pipe, admitted);

        if (pipe.jobs.empty() && pipe.active_readers == 0 && !pipe.writer_active)
            pipe.idle.notify_all();
    }
    submit_admitted(scheduler, pipe, admitted);
}

// Trampoline for an admitted pipe job (one per mode, so the mode needs no storage): the
// block travels as the entry's `data_` (ref adopted from the queue), the owning pipe via
// the block's `dispatch_arg` (free on this path -- a pipe block is created, dispatched once,
// and never `reset`, so its generation is always 0). Runs the body on this worker, then
// releases the pipe at body-return (the existing contract: nested tasks gate the task's
// COMPLETION, not the pipe) and re-dispatches. Mirrors `run_block_dispatch` -- no heap
// closure, so an async op allocates only its block.
void run_pipe_job(void* data, Access mode)
{
    Task_ptr block(static_cast<Task_control_block*>(data), Adopt_ref{});   // adopt the queued ref
    Pipe& pipe = *reinterpret_cast<Pipe*>(
        static_cast<std::uintptr_t>(block->dispatch_arg.load(std::memory_order_acquire)));

    block->execute(block, /*gen*/ 0);   // async blocks always have a body; claim(0) de-dups

    // This trampoline only ever runs on a worker of the scheduler the job was submitted to
    // (pipe blocks are not retractable and never inline-dispatched), so the ambient scheduler
    // is the right one for the re-dispatch. The pipe outlives this call: `wait_until_idle`
    // (Guarded's dtor) can't pass until the release below.
    release_and_redispatch(*current_scheduler, pipe, mode);
}   // `block` decrements here -> releases the ref the queue held

void run_pipe_job_read(void* data) { run_pipe_job(data, Access::read_only); }
void run_pipe_job_write(void* data) { run_pipe_job(data, Access::read_write); }

// Admit as many front jobs as the reader/writer rules allow, moving them into `admitted`
// for the caller to submit AFTER `pipe.mutex` is released. Caller holds `pipe.mutex`.
//   - readers: any number may run concurrently, but not alongside a writer
//   - writer: runs alone (no readers, no other writer)
//   - FIFO: a writer at the front holds back later jobs until prior readers drain
// Admission mutates the pipe state (reader count / writer flag) under the lock, so the
// admitted-but-not-yet-submitted window is invisible: `wait_until_idle` counts the job as
// active, and no concurrent `dispatch` can admit it twice (it left the deque). Submission
// happens outside the lock because a worker-less scheduler EXECUTES at submit -- a job body
// releasing this same pipe under the held mutex would deadlock (and shorter critical
// sections are better in every mode).
void dispatch(Pipe& pipe, Admitted& admitted)
{
    while (!pipe.jobs.empty())
    {
        Job& front = pipe.jobs.front();

        if (front.mode == Access::read_only)
        {
            if (pipe.writer_active)
                break;
            ++pipe.active_readers;
        }
        else
        {
            if (pipe.writer_active || pipe.active_readers > 0)
                break;
            pipe.writer_active = true;
#if TS_SAFETY_CHECKS
            pipe.write_epoch.fetch_add(1, std::memory_order_relaxed);   // write window opens
#endif
        }

        admitted.push_back(std::move(front));
        pipe.jobs.pop_front();
    }
}

// Submit one `dispatch` pass's admitted jobs, in admission order (FIFO preserved). Runs
// WITHOUT `pipe.mutex`.
void submit_admitted(Scheduler& scheduler, Pipe& pipe, Admitted& admitted)
{
    for (Job& job : admitted)
    {
        if (job.reservation)
        {
            // Signal the holder that it now owns the pipe; `writer_active`/`active_readers`
            // stay set (the reservation is released explicitly via `pipe_release`, not on
            // the callback's completion).
            submit_closure(scheduler, std::move(job.on_acquired), job.priority);
            continue;
        }
        // The `dispatch_arg` publish (release) is ordered before the submit that makes the
        // block reachable; one dispatch per pipe block, so no later publish exists to race.
        job.block->dispatch_arg.store(
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&pipe)),
            std::memory_order_release);
        Task_func_ptr trampoline = job.mode == Access::read_only ? &run_pipe_job_read
                                                                 : &run_pipe_job_write;
        // Hand the block's ref to the queue (release, no dec); the trampoline adopts it back.
        scheduler.submit(trampoline, job.block.release(), job.priority);
    }
}

} // namespace

void pipe_enqueue(Scheduler& scheduler, Pipe& pipe, Access mode, Task_ptr block, Priority priority)
{
    Admitted admitted;
    {
        std::scoped_lock lock(pipe.mutex);
        pipe.jobs.push_back(Job{ mode, /*reservation*/ false, priority, std::move(block), {} });
        dispatch(pipe, admitted);
    }
    submit_admitted(scheduler, pipe, admitted);
}

bool pipe_acquire(Scheduler& scheduler, Pipe& pipe, Access mode, std::move_only_function<void()> on_acquired)
{
    std::scoped_lock lock(pipe.mutex);
    // Admit at the front only if nothing is queued (FIFO) and the mode rule holds: a reader
    // joins concurrent readers (no writer), a writer needs the pipe idle.
    if (pipe.jobs.empty())
    {
        if (mode == Access::read_only)
        {
            if (!pipe.writer_active)
            {
                ++pipe.active_readers;   // acquired now; hold as a concurrent reader
                return true;
            }
        }
        else if (!pipe.writer_active && pipe.active_readers == 0)
        {
            pipe.writer_active = true;   // acquired now; hold as an exclusive writer
#if TS_SAFETY_CHECKS
            pipe.write_epoch.fetch_add(1, std::memory_order_relaxed);   // write window opens
#endif
            return true;
        }
    }
    // Deferred: sit behind the queued/active work; admitted (FIFO) when it drains. No
    // dispatch here -- the blocking condition still holds, so nothing can be admitted yet;
    // whatever releases it (a completing job or `pipe_release`) re-dispatches.
    pipe.jobs.push_back(Job{ mode, /*reservation*/ true, Priority::normal, {}, std::move(on_acquired) });
    return false;
}

void pipe_release(Scheduler& scheduler, Pipe& pipe, Access mode)
{
    release_and_redispatch(scheduler, pipe, mode);
}

void multi_acquire(Ref_ptr<Multi_async_state> state, Task_ptr block, std::size_t pos)
{
    if (pos == state->holds.size())
    {
        // All objects held -> run the body. A multi-async block is one-shot (never `reset`),
        // so reading its generation here is race-free (it is constant 0 for its lifetime).
        std::uint64_t gen = block->generation();
        submit_ready(std::move(block), gen);
        return;
    }

    auto [pipe, mode] = state->holds[pos];
    bool acquired = pipe_acquire(*state->scheduler, *pipe, mode,
        [state, block, pos]() mutable { multi_acquire(std::move(state), std::move(block), pos + 1); });

    if (acquired)
        multi_acquire(std::move(state), std::move(block), pos + 1);
}

// The tail pipe defines this in pipe_tail.cpp; the mutex pipe never reaches the
// `release()` branch that calls it (`pipe_count` is always 0 here), but the symbol must
// link in every configuration.
void pipe_enter_first(Task_control_block*)
{
    ts::fatal("pipe_enter_first without TS_PIPE_TAIL (a pipe_count was set on the mutex pipe)");
}

#if TS_SAFETY_CHECKS
// The `retract_or_wait` diagnostic (declared in task.h, defined here for the `Pipe`
// layout): the caller established that the wait is about to park, an access scope is
// active, and the target is non-retractable. Sharp message when the target is a
// single-object pipe job on a pipe this scope holds -- that shape deadlocks (the job is
// queued behind the very grant the waiter sits inside); general never-block warning
// otherwise (multi-object jobs land here too: their `dispatch_arg` carries the reuse
// generation, not a pipe, so the sharp match is out of reach for them).
void blocking_sync_diagnose(const Task_control_block* blk) noexcept
{
    if (blk->flags.pipe_job)
    {
        const Pipe* pipe = reinterpret_cast<const Pipe*>(
            static_cast<std::uintptr_t>(blk->dispatch_arg.load(std::memory_order_relaxed)));
        if (pipe && current_access && current_access->holds_epoch(&pipe->write_epoch))
        {
            TS_ENSURE(false, "sync() on an access to an object this scope already holds -- "
                "this deadlocks; use then/when_all or nested tasks");
            return;
        }
    }
    TS_ENSURE(false, "blocking sync() on non-retractable work inside an access scope -- "
        "occupies a worker and risks deadlock; prefer continuations (then/when_all) or "
        "nested tasks");
}
#endif

bool pipe_try_inline(Scheduler& scheduler, Pipe& pipe, Access mode, const Task_ptr& block)
{
    {
        std::scoped_lock lock(pipe.mutex);
        if (!pipe.jobs.empty())
            return false;   // queued work ahead -- preserve FIFO, defer to the queue
        if (mode == Access::read_only)
        {
            if (pipe.writer_active)
                return false;
            ++pipe.active_readers;   // join as a concurrent reader
        }
        else
        {
            if (pipe.writer_active || pipe.active_readers > 0)
                return false;
            pipe.writer_active = true;   // exclusive writer
#if TS_SAFETY_CHECKS
            pipe.write_epoch.fetch_add(1, std::memory_order_relaxed);   // write window opens
#endif
        }
    }

    // Admitted: run the body inline on THIS thread (it installs its own access scope). The
    // caller blocks for its duration. Then release + re-dispatch, mirroring `run_pipe_job`.
    block->execute(block, /*gen*/ 0);

    release_and_redispatch(scheduler, pipe, mode);
    return true;
}
#endif // !TS_PIPE_TAIL

} // namespace detail
} // namespace ts
