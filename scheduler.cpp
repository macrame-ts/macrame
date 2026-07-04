#include "scheduler.h"
#include "worker_thread.h"

#include <cstddef>
#include <cstdint>
#include <thread>

thread_local Scheduler* current_scheduler = nullptr;
thread_local int current_worker_index = -1;

namespace
{
// Cheap per-thread xorshift for random steal-victim selection (avoids every idle worker
// hammering the same victim).
std::uint32_t next_rand()
{
    static thread_local std::uint32_t s = 0x9e3779b9u ^ static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&s));
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
}

Scheduler::Scheduler(Scheduler_config config)
    : idle_policy_(config.idle_policy)
{
    uint32_t num_threads = config.num_threads;
    if (num_threads == 0)
        num_threads = std::thread::hardware_concurrency();

    // Per-worker deques must exist before any worker starts (workers steal from all of them).
    local_normal_.reserve(num_threads);
    for (uint32_t i = 0; i < num_threads; ++i)
        local_normal_.push_back(std::make_unique<detail::Work_stealing_deque<detail::Task_entry>>());

    workers_.reserve(num_threads);
    for (uint32_t i = 0; i < num_threads; ++i)
        workers_.emplace_back(*this, static_cast<int>(i));
}

Scheduler::~Scheduler()
{
    quit_.store(true, std::memory_order_release);

    // wake every parked worker so it observes `quit_` and drains/exits (a not-yet-parked
    // worker sees the bumped epoch in its re-check / commit_wait, so none is missed)
    if (idle_policy_ == Idle_policy::block)
        events_.notify_all();
}

void Scheduler::submit(Task_func_ptr func, void* data, Priority priority)
{
    // A worker submitting `normal` to its OWN scheduler pushes to its local deque (LIFO,
    // cache-hot, no shared cache line -- the producer fast path). External/non-normal submits,
    // and a full local deque, go to the global queue for the priority.
    if (priority == Priority::normal && current_scheduler == this && current_worker_index >= 0
        && local_normal_[static_cast<std::size_t>(current_worker_index)]->push({ func, data }))
    {
        if (idle_policy_ == Idle_policy::block)
            events_.notify_one();   // wake a thief to help (no-op if none parked)
        return;
    }

    queues_[static_cast<std::size_t>(priority)].push({ func, data });

    if (idle_policy_ == Idle_policy::block)
        events_.notify_one();
}

// Serve a `low` task after this many consecutive high/normal tasks, so a steady stream of
// normal work cannot starve low indefinitely (aging valve, à la Go/Tokio's "check global
// every ~61/31"). Low priority still means low: it just gets guaranteed occasional progress.
constexpr int low_valve_threshold = 64;

// Find one task for `worker_index`: global high (strict) -> own local deque (LIFO) -> global
// normal -> global low -> steal `normal` from a random victim. A per-worker aging counter
// forces `low` ahead of normal once in a while (the starvation valve).
bool Scheduler::find_work(int worker_index, detail::Task_entry& out)
{
    static thread_local int since_low = 0;   // consecutive high/normal tasks taken by this worker

    if (queues_[0].pop(out))                                   // global high (strict)
    {
        ++since_low;
        return true;
    }
    // Valve: if we've taken many high/normal in a row, serve one low BEFORE normal/local.
    if (since_low >= low_valve_threshold && queues_[2].pop(out))
    {
        since_low = 0;
        return true;
    }
    if (local_normal_[static_cast<std::size_t>(worker_index)]->take(out))   // own deque, LIFO
    {
        ++since_low;
        return true;
    }
    if (queues_[1].pop(out))                                   // global normal (overflow + external)
    {
        ++since_low;
        return true;
    }
    if (queues_[2].pop(out))                                   // global low (normal was empty anyway)
    {
        since_low = 0;
        return true;
    }

    // Steal a `normal` task from another worker's deque (start at a random victim, scan around).
    int n = static_cast<int>(local_normal_.size());
    if (n > 1)
    {
        int start = static_cast<int>(next_rand() % static_cast<std::uint32_t>(n));
        for (int i = 0; i < n; ++i)
        {
            int v = start + i;
            if (v >= n)
                v -= n;
            if (v == worker_index)
                continue;
            if (local_normal_[static_cast<std::size_t>(v)]->steal(out))
            {
                ++since_low;
                return true;
            }
        }
    }
    return false;
}

int Scheduler::worker_count() const noexcept
{
    return static_cast<int>(workers_.size());
}

bool Scheduler::all_empty() const
{
    for (const auto& q : queues_)
        if (!q.empty())
            return false;
    for (const auto& d : local_normal_)
        if (!d->empty())
            return false;
    return true;
}
