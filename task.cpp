#include "task.h"

// Dynamic_task

void Dynamic_task::submit(Scheduler& scheduler)
{
    auto trampoline = [](void* data)
    {
        reinterpret_cast<Dynamic_task*>(data)->task_func_();
    };
    scheduler.submit(trampoline, this, priority_);
}

// Awaitable_task

Awaitable_task::Awaitable_task(std::move_only_function<void()> task_func, Priority priority)
    : task_func_(std::move(task_func))
    , priority_(priority)
{}

void Awaitable_task::submit(Scheduler& scheduler)
{
    auto trampoline = [](void* data)
    {
        auto task = reinterpret_cast<Awaitable_task*>(data);
        task->task_func_();
        task->sem_.release();
    };
    scheduler.submit(trampoline, this, priority_);
}

void Awaitable_task::wait()
{
    sem_.acquire();
}

// Ref_counted_task_impl

Ref_counted_task_impl::Ref_counted_task_impl(std::move_only_function<void()> task_func, Priority priority)
    : task_func_(std::move(task_func))
    , priority_(priority)
{}

void Ref_counted_task_impl::submit(Scheduler& scheduler)
{
    auto func = [](void* data)
    {
        auto task = reinterpret_cast<Ref_counted_task_impl*>(data);
        task->task_func_();
        task->release();
    };
    scheduler.submit(func, this, priority_);
    add_ref();
}

// Dependable_task_impl

Dependable_task_impl::Dependable_task_impl(std::move_only_function<void()> task_func, Priority priority)
    : task_func_(std::move(task_func))
    , priority_(priority)
{}

void Dependable_task_impl::then(Scheduler& scheduler, const Dependable_task& subsequent)
{
    std::unique_lock lock(subsequents_mutex_);
    if (subsequents_closed_)
    {
        lock.unlock();
        subsequent->submit(scheduler);
    }
    else
        subsequents_.push_back(subsequent);
}

void Dependable_task_impl::submit(Scheduler& scheduler)
{
    if (prerequisites_.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;

    add_ref();
    auto func = [](void* data)
    {
        auto task = reinterpret_cast<Dependable_task_impl*>(data);
        task->task_func_();

        decltype(task->subsequents_) subsequents;
        {
            std::scoped_lock lock(task->subsequents_mutex_);
            task->subsequents_closed_ = true;
            subsequents = std::move(task->subsequents_);
        }

        for (auto& subsequent : subsequents)
            subsequent->submit(*current_scheduler);

        task->release();
    };
    scheduler.submit(func, this, priority_);
}
