#include "ts/detail/worker_thread.h"

namespace ts::detail
{

Worker_thread::Worker_thread(Scheduler& scheduler, int index)
{
    thread_ = std::jthread{ [&scheduler, index]{ main(scheduler, index); } };
}

void Worker_thread::main(Scheduler& scheduler, int index)
{
    current_scheduler = &scheduler;
    current_worker_index = index;

    while (true)
    {
        detail::Task_entry task;
        if (scheduler.find_work(index, task))   // local + globals + steal
        {
            task.func_(task.data_);
            continue;
        }

        // No work found anywhere.
        if (scheduler.quit_.load(std::memory_order_acquire) && scheduler.all_empty())
            break;

        // Idle wait for the configured policy (park / spin-then-park / handoff / spin). Returns
        // true with a task to run; false means re-scan and re-check quit at the top.
        if (scheduler.wait_for_work(index, task))
            task.func_(task.data_);
    }

    current_worker_index = -1;
    current_scheduler = nullptr;
}

} // namespace ts::detail
