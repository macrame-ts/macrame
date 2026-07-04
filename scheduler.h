#pragma once

#include "mpmc_queue.h"
#include "priority.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <semaphore>
#include <vector>

enum class Idle_policy
{
    spin,   // spin-wait on the queue; lowest wake latency, burns a core per idle worker
    block,  // sleep on a counting semaphore while the queue is empty
};

struct Scheduler_config
{
    uint32_t num_threads = 0;                    // 0 -> `std::thread::hardware_concurrency()`
    Idle_policy idle_policy = Idle_policy::block;
};

using Task_func_ptr = void(*)(void* data);

namespace detail
{

// A queued task: the func + its data. Priority is no longer a field -- it is the queue the
// task lives in (one lock-free MPMC queue per priority, scanned high->low).
struct Task_entry
{
    Task_func_ptr func_ = nullptr;
    void* data_ = nullptr;
};

class Worker_thread;

// One queue per `Priority` value (high/normal/low), indexed by the enum.
inline constexpr std::size_t priority_count = 3;

} // namespace detail

class Scheduler;
extern thread_local Scheduler* current_scheduler;

class Scheduler
{
    friend class detail::Worker_thread;

public:
    Scheduler(Scheduler_config config = {});
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void submit(Task_func_ptr func, void* data, Priority priority = Priority::normal);

private:
    // Scan the per-priority queues high->low; true if a task was popped.
    bool try_pop(detail::Task_entry& out);
    // Approximate: all queues empty (racy; for the shutdown-drain check with `quit_`).
    bool all_empty() const;

    // One lock-free MPMC queue per priority (index = the `Priority` enum value).
    std::array<detail::Mpmc_queue<detail::Task_entry>, detail::priority_count> queues_;
    std::atomic<bool> quit_ = false;
    std::counting_semaphore<> work_available_{ 0 };
    const Idle_policy idle_policy_;
    std::vector<detail::Worker_thread> workers_;
};
