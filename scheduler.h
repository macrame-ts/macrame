#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <semaphore>
#include <vector>

enum class Priority { high, normal, low };

enum class Idle_policy
{
    spin,   // busy-spin; lowest wake latency, burns a core per idle worker
    block,  // sleep on a counting semaphore while the queue is empty
};

struct Scheduler_config
{
    uint32_t num_threads = 0;                    // 0 -> std::thread::hardware_concurrency()
    Idle_policy idle_policy = Idle_policy::block;
};

using Task_func_ptr = void(*)(void* data);

namespace detail
{

struct Task_skeleton
{
    Task_func_ptr func_;
    void* data_;
    Priority priority_;
};

struct Task_queue_cmp
{
    bool operator()(const Task_skeleton& l, const Task_skeleton& r) const
    {
        return l.priority_ > r.priority_;
    }
};

using Task_queue = std::priority_queue<Task_skeleton, std::vector<Task_skeleton>, Task_queue_cmp>;

class Worker_thread;

} // namespace detail

class Scheduler;
extern thread_local Scheduler* current_scheduler;

class Scheduler
{
    friend class detail::Worker_thread;

public:
    Scheduler(Scheduler_config config = {});
    ~Scheduler();

    void submit(Task_func_ptr func, void* data, Priority priority = Priority::normal);

private:
    detail::Task_queue task_queue_;
    std::mutex queue_mutex_;
    std::atomic<bool> quit_ = false;
    std::counting_semaphore<> work_available_{ 0 };
    const Idle_policy idle_policy_;
    std::vector<detail::Worker_thread> workers_;
};
