#include "ts/detail/worker_thread.h"

namespace ts::detail
{

Worker_thread::Worker_thread(Scheduler& scheduler, int index)
{
    thread_ = std::jthread{ [&scheduler, index]{ main(scheduler, index); } };
}

void Worker_thread::main(Scheduler& scheduler, int index)
{
    current_worker_index = index;

    // Busy/idle state for the scheduler's quiescence signal (`Scheduler::quiescent`, the
    // deadlock net). Tracked as a transition, not as "parked": under `Idle_policy::spin` a
    // worker never parks, so it marks itself idle once on running dry and clears that only
    // when it finds work again. Two atomics per busy<->idle crossing, none per task.
    bool idle = false;
    auto mark = [&scheduler, &idle](bool now_idle)
    {
        if (idle == now_idle)
            return;
        idle = now_idle;
        if (now_idle)
            scheduler.worker_idle();
        else
            scheduler.worker_busy();
    };

    while (true)
    {
        detail::Task_entry task;
        if (scheduler.find_work_dispatch(index, task))   // local + globals + steal (times a hit as machinery)
        {
            mark(false);
            scheduler.run_task(index, task);
            continue;
        }

        // No work found anywhere.
        if (scheduler.quit_.load(std::memory_order_acquire) && scheduler.all_empty())
            break;

        mark(true);
        // Idle wait for the configured policy (park / spin-then-park / handoff / spin). Returns
        // true with a task to run; false means re-scan and re-check quit at the top.
        if (scheduler.wait_for_work(index, task))
        {
            mark(false);
            scheduler.run_task(index, task);
        }
    }

    mark(false);   // leaving the pool: no longer a worker that could be idle
    current_worker_index = -1;
}

} // namespace ts::detail
