#pragma once

#include "event_count.h"
#include "mpmc_queue.h"
#include "priority.h"
#include "work_stealing_deque.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
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
// This thread's worker index within `current_scheduler` (>= 0 for a worker of it, else -1).
// Routes a worker's own `normal` submits to its local deque, and identifies it in stealing.
extern thread_local int current_worker_index;

class Scheduler
{
    friend class detail::Worker_thread;

public:
    Scheduler(Scheduler_config config = {});
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void submit(Task_func_ptr func, void* data, Priority priority = Priority::normal);

    // Number of worker threads (the natural default concurrency for `parallel_for`).
    // Defined in the .cpp -- `Worker_thread` is incomplete here.
    int worker_count() const noexcept;

private:
    // Find one task for worker `worker_index`, scanning: global high -> its own local deque
    // (LIFO, cache-hot) -> global normal -> global low -> steal `normal` from a random victim.
    // High stays strict (checked first). True if a task was found.
    bool find_work(int worker_index, detail::Task_entry& out);
    // Approximate: all global queues AND all local deques empty (racy; shutdown-drain check).
    bool all_empty() const;

    // One lock-free MPMC queue per priority (index = the `Priority` enum value). `high`/`low`
    // are global-only; `normal` also has per-worker deques (below), with this as overflow +
    // the injector for external (non-worker) submits.
    std::array<detail::Mpmc_queue<detail::Task_entry>, detail::priority_count> queues_;
    // Per-worker Chase-Lev deque for `normal`: a worker's own `normal` submits go here (LIFO,
    // no shared cache line -- the producer fast path); thieves steal FIFO. One per worker.
    std::vector<std::unique_ptr<detail::Work_stealing_deque<detail::Task_entry>>> local_normal_;
    std::atomic<bool> quit_ = false;
    detail::Event_count events_;   // wakes idle (block-mode) workers; replaces the semaphore
    const Idle_policy idle_policy_;
    std::vector<detail::Worker_thread> workers_;
};
