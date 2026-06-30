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
        // `block` mode: sleep until a permit is available. permit count tracks
        // queued tasks, so a successful acquire implies work (or a shutdown
        // wake, caught by the `quit_` check below).
        if (scheduler.idle_policy_ == Idle_policy::block)
            scheduler.work_available_.acquire();

        std::unique_lock lock(scheduler.queue_mutex_);
        if (scheduler.quit_.load(std::memory_order_acquire) && scheduler.task_queue_.empty())
            break;

        if (scheduler.task_queue_.empty())
        {
            // `spin` mode reaches here on an empty queue; yield instead of
            // re-locking immediately and starving submitters
            lock.unlock();
            std::this_thread::yield();
            continue;
        }

        auto task = scheduler.task_queue_.top();
        scheduler.task_queue_.pop();
        lock.unlock();

        task.func_(task.data_);
    }

    current_scheduler = nullptr;
}

} // namespace detail
