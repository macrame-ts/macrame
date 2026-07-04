#include "worker_thread.h"

namespace detail
{

Worker_thread::Worker_thread(Scheduler& scheduler)
{
    thread_ = std::jthread{ [&scheduler]{ main(scheduler); } };
}

void Worker_thread::main(Scheduler& scheduler)
{
    current_scheduler = &scheduler;

    while (true)
    {
        // `block` mode: sleep until a permit is available. The permit count tracks queued
        // tasks (+ N shutdown wakes), so a successful acquire entitles this worker to exactly
        // one task -- popped below without re-acquiring on a transient scan miss.
        if (scheduler.idle_policy_ == Idle_policy::block)
            scheduler.work_available_.acquire();

        if (scheduler.quit_.load(std::memory_order_acquire) && scheduler.all_empty())
            break;

        // Take exactly one task. Retry the high->low scan on a transient miss (another worker
        // grabbed the task this permit is for; the count guarantees one still remains) rather
        // than re-acquiring. Bail if the scheduler is shutting down and the queues have drained.
        detail::Task_entry task;
        bool got = false;
        for (;;)
        {
            if (scheduler.try_pop(task))
            {
                got = true;
                break;
            }
            if (scheduler.quit_.load(std::memory_order_acquire) && scheduler.all_empty())
                break;   // shutting down and drained
            std::this_thread::yield();
        }

        // Inner loop bailed without a task -> shutdown: exit. Falling back to `acquire()`
        // here would burn a second permit for no work (a leak -> another worker blocks on
        // acquire forever). This transition is where a block-mode shutdown deadlocked.
        if (!got)
            break;

        task.func_(task.data_);
    }

    current_scheduler = nullptr;
}

} // namespace detail
