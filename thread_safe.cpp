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

void submit_closure(Scheduler& scheduler, std::move_only_function<void()> closure)
{
    auto* held = new std::move_only_function<void()>(std::move(closure));
    scheduler.submit(
        [](void* data)
        {
            auto* fn = static_cast<std::move_only_function<void()>*>(data);
            (*fn)();
            delete fn;
        },
        held);
}

namespace
{

void dispatch(Scheduler& scheduler, Pipe& pipe);

// Run one job as its own scheduler task; on completion, update pipe state and
// re-dispatch. Because each job is an independent task, consecutive readers run
// in parallel; a writer runs alone.
void submit_job(Scheduler& scheduler, Pipe& pipe, Job job)
{
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
        });
}

// Admit as many front jobs as the reader/writer rules allow. Caller holds pipe.mutex.
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
        submit_job(scheduler, pipe, std::move(job));
    }
}

} // namespace

void pipe_enqueue(Scheduler& scheduler, Pipe& pipe, Access mode, std::move_only_function<void()> fn)
{
    std::scoped_lock lock(pipe.mutex);
    pipe.jobs.push_back(Job{ mode, std::move(fn) });
    dispatch(scheduler, pipe);
}

} // namespace detail
} // namespace ts
