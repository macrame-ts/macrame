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
        detail::Task_entry task;
        if (scheduler.try_pop(task))   // high->low scan
        {
            task.func_(task.data_);
            continue;
        }

        // No work found.
        if (scheduler.quit_.load(std::memory_order_acquire) && scheduler.all_empty())
            break;

        if (scheduler.idle_policy_ == Idle_policy::block)
        {
            // Park until notified -- but snapshot the epoch and RE-CHECK first, so work (or a
            // shutdown) that raced the failed scan isn't missed: if it enqueued+notified
            // before the snapshot, the re-check finds it; if after, `commit_wait` returns at
            // once (epoch moved). See `Event_count`.
            std::uint32_t key = scheduler.events_.prepare_wait();
            if (scheduler.try_pop(task))
            {
                scheduler.events_.cancel_wait();
                task.func_(task.data_);
                continue;
            }
            if (scheduler.quit_.load(std::memory_order_acquire))
            {
                scheduler.events_.cancel_wait();
                continue;   // -> the quit/all_empty break at the top of the loop
            }
            scheduler.events_.commit_wait(key);
        }
        else
        {
            std::this_thread::yield();   // spin mode: never park
        }
    }

    current_scheduler = nullptr;
}

} // namespace detail
