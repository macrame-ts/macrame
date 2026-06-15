#pragma once

#include "scheduler.h"

#include <atomic>
#include <functional>
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

    void submit(Scheduler& scheduler);
};

class Awaitable_task
{
private:
    std::move_only_function<void()> task_func_;
    Priority priority_;

    std::binary_semaphore sem_{ 0 };

public:
    Awaitable_task(std::move_only_function<void()> task_func, Priority priority = Priority::normal);

    void submit(Scheduler& scheduler);
    void wait();
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
        if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete static_cast<const T*>(this);
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
        if (this != &other)
        {
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

    Ref_counted_task_impl(std::move_only_function<void()> task_func, Priority priority = Priority::normal);

    void submit(Scheduler& scheduler);
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
    Dependable_task_impl(std::move_only_function<void()> task_func, Priority priority = Priority::normal);

    void then(Scheduler& scheduler, const Dependable_task& subsequent);
    void submit(Scheduler& scheduler);
};
