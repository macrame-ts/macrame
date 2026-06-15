#pragma once

#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

enum class Priority { high, normal, low };

using Task_func_ptr = void(*)(void* data);

class Scheduler;
extern thread_local Scheduler* current_scheduler;

class Scheduler {
private:
    struct Task_skeleton
    {
        Task_func_ptr func_;
        void* data_;
        Priority priority_;
    };

    using Cmp = decltype([](const Task_skeleton& l, const Task_skeleton r)
    { return l.priority_ > r.priority_; });
    std::priority_queue<Task_skeleton, std::vector<Task_skeleton>, Cmp> task_queue;
    std::mutex queue_mutex;

    std::jthread worker_;
    std::atomic<bool> quit_ = false;

public:
    Scheduler();
    ~Scheduler();

    void submit(Task_func_ptr func, void* data, Priority priority = Priority::normal);

private:
    void worker_main();
};
