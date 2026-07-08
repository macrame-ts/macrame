#include "guarded.h"

#include <cstdint>

namespace ts
{

Scheduler& default_scheduler()
{
    static Scheduler scheduler;
    return scheduler;
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
    default_scheduler().submit(&run_block_dispatch, block.release(), priority);
}

namespace
{

void dispatch(Scheduler& scheduler, Pipe& pipe);

// Release the pipe in `mode` and admit whatever the release unblocks; notify a
// `wait_until_idle` waiter if the pipe drained. The tail shared by every way a pipe access
// ends (queued job body returning, inline body returning, `pipe_release`).
void release_and_redispatch(Scheduler& scheduler, Pipe& pipe, Access mode)
{
    std::scoped_lock lock(pipe.mutex);
    if (mode == Access::read_only)
        --pipe.active_readers;
    else
        pipe.writer_active = false;

    dispatch(scheduler, pipe);

    if (pipe.jobs.empty() && pipe.active_readers == 0 && !pipe.writer_active)
        pipe.idle.notify_all();
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

// Hand an admitted job to the scheduler. Caller holds `pipe.mutex` (orders the
// `dispatch_arg` publish before any later dispatch of the same block could exist -- there
// is none: one dispatch per pipe block).
void submit_job(Scheduler& scheduler, Pipe& pipe, Job job)
{
    job.block->dispatch_arg.store(
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&pipe)),
        std::memory_order_release);
    Task_func_ptr trampoline = job.mode == Access::read_only ? &run_pipe_job_read
                                                             : &run_pipe_job_write;
    // Hand the block's ref to the queue (release, no dec); the trampoline adopts it back.
    scheduler.submit(trampoline, job.block.release(), job.priority);
}

// Admit as many front jobs as the reader/writer rules allow. Caller holds `pipe.mutex`.
//   - readers: any number may run concurrently, but not alongside a writer
//   - writer: runs alone (no readers, no other writer)
//   - FIFO: a writer at the front holds back later jobs until prior readers drain
void dispatch(Scheduler& scheduler, Pipe& pipe)
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
        }

        Job job = std::move(front);
        pipe.jobs.pop_front();

        if (job.reservation)
            // Signal the holder that it now owns the pipe; leave `writer_active` set
            // (the reservation is released explicitly via `pipe_release`, not on the
            // callback's completion).
            submit_closure(scheduler, std::move(job.on_acquired), job.priority);
        else
            submit_job(scheduler, pipe, std::move(job));
    }
}

} // namespace

void pipe_enqueue(Scheduler& scheduler, Pipe& pipe, Access mode, Task_ptr block,
                  Priority priority)
{
    std::scoped_lock lock(pipe.mutex);
    pipe.jobs.push_back(Job{ mode, /*reservation*/ false, priority, std::move(block), {} });
    dispatch(scheduler, pipe);
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

void multi_acquire(Ref_ptr<Multi_async_state> state,
                   Task_ptr block, std::size_t pos)
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
        }
    }

    // Admitted: run the body inline on THIS thread (it installs its own access scope). The
    // caller blocks for its duration. Then release + re-dispatch, mirroring `run_pipe_job`.
    block->execute(block, /*gen*/ 0);

    release_and_redispatch(scheduler, pipe, mode);
    return true;
}

} // namespace detail
} // namespace ts
