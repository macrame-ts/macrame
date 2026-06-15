#include "scheduler.h"

#include <functional>
#include <iostream>
#include <mutex>
#include <semaphore>
#include <vector>

template<typename Task_func_t>
struct Static_task 
{
    Task_func_t task_func_;
    Priority priority_ = Priority::normal;

    void submit(Scheduler& scheduler) {
        auto trampoline = [](void* data) 
        {
            reinterpret_cast<Static_task*>(data)->task_func_();
        };
        scheduler.submit(trampoline, this, priority_);
    }
};

struct Dynamic_task 
{
    std::move_only_function<void()> task_func_;
    Priority priority_ = Priority::normal;

    void submit(Scheduler& scheduler) 
    {
        auto trampoline = [](void* data) 
        {
            reinterpret_cast<Dynamic_task*>(data)->task_func_();
        };
        scheduler.submit(trampoline, this, priority_);
    }
};

class Awaitable_task 
{
private:
    std::move_only_function<void()> task_func_;
    Priority priority_;

    std::binary_semaphore sem_{ 0 };

public:
    Awaitable_task(std::move_only_function<void()> task_func, Priority priority = Priority::normal)
        : task_func_(std::move(task_func))
        , priority_(priority)
    {}

    void submit(Scheduler& scheduler) {
        auto trampoline = [](void* data) {
            auto task = reinterpret_cast<Awaitable_task*>(data);
            task->task_func_();
            task->sem_.release();
        };
        scheduler.submit(trampoline, this, priority_);
    }

    void wait() {
        sem_.acquire();
    }
};

template <typename T>
class Ref_counted_base 
{
private:
    mutable std::atomic<uint32_t> ref_count_{0};

protected:
    ~Ref_counted_base() = default;

public:
    Ref_counted_base() = default;

    Ref_counted_base(const Ref_counted_base&) noexcept 
        : ref_count_(0) 
    {}
    Ref_counted_base& operator=(const Ref_counted_base&) noexcept 
    { return *this; }

    void add_ref() const noexcept 
    {
        ref_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void release() const noexcept 
    {
        if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete static_cast<const T*>(this);
        }
    }

    uint32_t ref_count() const noexcept 
    {
        return ref_count_.load(std::memory_order_relaxed);
    }
};

template <typename T>
class Ref_counted_ptr 
{
private:
    T* ptr_ = nullptr;

    Ref_counted_ptr(T* ptr) noexcept 
        : ptr_(ptr) 
    { 
        if (ptr_) 
            ptr_->add_ref(); 
    }

public:
    Ref_counted_ptr() noexcept = default;

    template<typename... Args>
    Ref_counted_ptr(Args&&... args)
        : Ref_counted_ptr(new T{ std::forward<Args>(args)... })
    {}

    ~Ref_counted_ptr() 
    { 
        if (ptr_) 
            ptr_->release(); 
    }

    Ref_counted_ptr(const Ref_counted_ptr& other) noexcept 
        : ptr_(other.ptr_) 
    {
        if (ptr_) 
            ptr_->add_ref();
    }

    Ref_counted_ptr& operator=(const Ref_counted_ptr& other) noexcept 
    {
        if (this != &other) 
        {
            if (ptr_) 
                ptr_->release();
            ptr_ = other.ptr_;
            if (ptr_) 
                ptr_->add_ref();
        }
        return *this;
    }

    Ref_counted_ptr(Ref_counted_ptr&& other) noexcept 
        : ptr_(other.ptr_) 
    {
        other.ptr_ = nullptr;
    }

    Ref_counted_ptr& operator=(Ref_counted_ptr&& other) noexcept 
    {
        if (this != &other) {
            if (ptr_) 
                ptr_->release();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
};

struct Ref_counted_task_impl : public Ref_counted_base<Ref_counted_task_impl> 
{
    std::move_only_function<void()> task_func_;
    Priority priority_;

    Ref_counted_task_impl(std::move_only_function<void()> task_func, Priority priority = Priority::normal)
        : task_func_(std::move(task_func))
        , priority_(priority)
    {}

    void submit(Scheduler& scheduler)
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
};

using Ref_counted_task = Ref_counted_ptr<Ref_counted_task_impl>;

class Dependable_task_impl;
using Dependable_task = Ref_counted_ptr<Dependable_task_impl>;
class Dependable_task_impl : public Ref_counted_base<Dependable_task_impl>
{
private:
    std::move_only_function<void()> task_func_;
    Priority priority_;

    std::vector<Dependable_task> subsequents_;
    bool subsequents_closed_ = false;
    std::mutex subsequents_mutex_;

    std::atomic<uint32_t> prerequisites_ = 1;

public:
    Dependable_task_impl(std::move_only_function<void()> task_func, Priority priority = Priority::normal)
        : task_func_(std::move(task_func))
        , priority_(priority)
    {}

    void then(Scheduler& scheduler, const Dependable_task& subsequent)
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

    void submit(Scheduler& scheduler)
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
            {
                subsequent->submit(*current_scheduler);
            }

            task->release();
        };
        scheduler.submit(func, this, priority_);
    }
};

int main() 
{
    Scheduler scheduler;

    Static_task static_task{ []{ std::cout << "static task\n"; } };
    static_task.submit(scheduler);

    Dynamic_task dynamic_task{ []{ std::cout << "dynamic task\n"; }, Priority::high };
    dynamic_task.submit(scheduler);

    scheduler.submit([](void*) { std::cout << "row task\n"; }, nullptr);

    Awaitable_task awaitable_task{ []{ std::cout << "awaitable task\n"; } };
    awaitable_task.submit(scheduler);
    awaitable_task.wait();

    Ref_counted_task ref_counted_task{ []{ std::cout << "ref-counted task\n"; } };
    ref_counted_task->submit(scheduler);

    {
        Dependable_task prereq_task{ []{ std::cout << "prereq task 1\n"; } };
        prereq_task->then(scheduler, {[]{ std::cout << "subseq task 1\n"; } });
        prereq_task->submit(scheduler);
    }

    {
        Dependable_task prereq_task{ []{ std::cout << "prereq task 2\n"; } };
        prereq_task->submit(scheduler);
        prereq_task->then(scheduler, {[]{ std::cout << "subseq task 2\n"; } });
    }
}
