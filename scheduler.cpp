#include "scheduler.h"

thread_local Scheduler* current_scheduler = nullptr;

Scheduler::Scheduler()
{
    worker_ = std::jthread{ [this]{ worker_main(); } };
}

Scheduler::~Scheduler()
{
    quit_.store(true, std::memory_order_release);
}

void Scheduler::submit(Task_func_ptr func, void* data, Priority priority)
{
    std::lock_guard lock(queue_mutex);
    task_queue.emplace(func, data, priority);
}

void Scheduler::worker_main()
{
    current_scheduler = this;

    while (true)
    {
        std::unique_lock lock(queue_mutex);
        if (quit_.load(std::memory_order_acquire) && task_queue.empty())
            break;

        if (task_queue.empty())
            continue;

        auto task = task_queue.top();
        task_queue.pop();
        lock.unlock();

        task.func_(task.data_);
    }

    current_scheduler = nullptr;
}
