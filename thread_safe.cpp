#include "thread_safe.h"

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

// A block whose prerequisites are all met: schedule it to run (its body, or, if
// bodyless, just complete). Bridges task.h's lock-counter to the scheduler.
void submit_ready(std::shared_ptr<Task_control_block> block)
{
    // Capture the reuse generation: a dispatch left stale by a `reset` (retraction can
    // queue a duplicate) must not re-run the body against the new run. `claim(gen)`
    // (inside execute, or here for a bodyless block) is the atomic gate. See task.h.
    std::uint64_t gen = block->generation();
    Priority priority = block->flags.priority;
    submit_closure(default_scheduler(),
        [block = std::move(block), gen]
        {
            if (block->execute)
                block->execute(block, gen);      // claims `gen` internally (dedup + stale-skip)
            else if (block->claim(gen))          // bodyless: claim so a stale/duplicate no-ops
                block->complete();
        },
        priority);
}

namespace
{

void dispatch(Scheduler& scheduler, Pipe& pipe);

// Run one job as its own scheduler task; on completion, update pipe state and
// re-dispatch. Because each job is an independent task, consecutive readers run
// in parallel; a writer runs alone.
void submit_job(Scheduler& scheduler, Pipe& pipe, Job job)
{
    Priority priority = job.priority;
    submit_closure(scheduler,
        [&scheduler, &pipe, job = std::move(job)]() mutable
        {
            job.fn();

            std::scoped_lock lock(pipe.mutex);
            if (job.mode == Access::read_only)
                --pipe.active_readers;
            else
                pipe.writer_active = false;

            dispatch(scheduler, pipe);

            if (pipe.jobs.empty() && pipe.active_readers == 0 && !pipe.writer_active)
                pipe.idle.notify_all();
        },
        priority);
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
            submit_closure(scheduler, std::move(job.fn), job.priority);
        else
            submit_job(scheduler, pipe, std::move(job));
    }
}

} // namespace

void pipe_enqueue(Scheduler& scheduler, Pipe& pipe, Access mode, std::move_only_function<void()> fn,
                  Priority priority)
{
    std::scoped_lock lock(pipe.mutex);
    pipe.jobs.push_back(Job{ mode, std::move(fn), /*reservation*/ false, priority });
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
    pipe.jobs.push_back(Job{ mode, std::move(on_acquired), /*reservation*/ true });
    return false;
}

void pipe_release(Scheduler& scheduler, Pipe& pipe, Access mode)
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

void multi_acquire(std::shared_ptr<Multi_async_state> state,
                   std::shared_ptr<Task_control_block> block, std::size_t pos)
{
    if (pos == state->holds.size())
    {
        submit_ready(std::move(block));   // all objects held -> run the body
        return;
    }

    auto [pipe, mode] = state->holds[pos];
    bool acquired = pipe_acquire(*state->scheduler, *pipe, mode,
        [state, block, pos]() mutable { multi_acquire(std::move(state), std::move(block), pos + 1); });

    if (acquired)
        multi_acquire(std::move(state), std::move(block), pos + 1);
}

bool pipe_try_inline(Scheduler& scheduler, Pipe& pipe, Access mode, std::move_only_function<void()>& fn)
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
    // caller blocks for its duration. Then release + re-dispatch, mirroring `submit_job`.
    fn();

    std::scoped_lock lock(pipe.mutex);
    if (mode == Access::read_only)
        --pipe.active_readers;
    else
        pipe.writer_active = false;
    dispatch(scheduler, pipe);
    if (pipe.jobs.empty() && pipe.active_readers == 0 && !pipe.writer_active)
        pipe.idle.notify_all();
    return true;
}

} // namespace detail
} // namespace ts
