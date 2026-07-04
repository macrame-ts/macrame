#include "scheduler.h"
#include "worker_thread.h"

#include <cstddef>
#include <thread>

thread_local Scheduler* current_scheduler = nullptr;

Scheduler::Scheduler(Scheduler_config config)
    : idle_policy_(config.idle_policy)
{
    uint32_t num_threads = config.num_threads;
    if (num_threads == 0)
        num_threads = std::thread::hardware_concurrency();

    workers_.reserve(num_threads);
    for (uint32_t i = 0; i < num_threads; ++i)
        workers_.emplace_back(*this);
}

Scheduler::~Scheduler()
{
    quit_.store(true, std::memory_order_release);

    // wake every blocked worker so it observes `quit_` and drains/exits
    if (idle_policy_ == Idle_policy::block)
        work_available_.release(static_cast<std::ptrdiff_t>(workers_.size()));
}

void Scheduler::submit(Task_func_ptr func, void* data, Priority priority)
{
    queues_[static_cast<std::size_t>(priority)].push({ func, data });

    if (idle_policy_ == Idle_policy::block)
        work_available_.release();
}

// Scan the per-priority queues high (index 0) -> low; take the first available task.
bool Scheduler::try_pop(detail::Task_entry& out)
{
    for (std::size_t p = 0; p < detail::priority_count; ++p)
        if (queues_[p].pop(out))
            return true;
    return false;
}

bool Scheduler::all_empty() const
{
    for (const auto& q : queues_)
        if (!q.empty())
            return false;
    return true;
}
