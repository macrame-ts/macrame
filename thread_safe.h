#pragma once

#include "access.h"
#include "scheduler.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <type_traits>
#include <utility>

namespace ts
{

// Ambient scheduler used by async() (v1: a process-wide default).
Scheduler& default_scheduler();

namespace detail
{

struct Job
{
    Access mode;
    std::move_only_function<void()> fn;
};

// A per-object reader/writer pipe. Jobs are admitted in FIFO order; consecutive
// readers run concurrently (each is its own scheduler task), a writer runs alone
// (no readers, no other writer). Different objects have independent pipes and run
// in parallel. Non-blocking: callers never wait; admission is completion-driven.
struct Pipe
{
    std::mutex mutex;
    std::condition_variable idle;
    std::deque<Job> jobs;
    int active_readers = 0;
    bool writer_active = false;

    // Blocks until the pipe is fully drained and nothing is in flight.
    void wait_until_idle()
    {
        std::unique_lock lock(mutex);
        idle.wait(lock, [this]
        {
            return jobs.empty() && active_readers == 0 && !writer_active;
        });
    }
};

void pipe_enqueue(Scheduler& scheduler, Pipe& pipe, Access mode, std::move_only_function<void()> fn);

// --- Task completion state -------------------------------------------------

template<typename R>
struct Task_state
{
    std::binary_semaphore done{ 0 };
    std::atomic<bool> ready{ false };
    std::optional<R> result;

    template<typename Fn, typename Arg>
    void run(Fn& fn, Arg& arg)
    {
        result.emplace(fn(arg));
        ready.store(true, std::memory_order_release);
        done.release();
    }

    R get()
    {
        done.acquire();
        return std::move(*result);
    }
};

template<>
struct Task_state<void>
{
    std::binary_semaphore done{ 0 };
    std::atomic<bool> ready{ false };

    template<typename Fn, typename Arg>
    void run(Fn& fn, Arg& arg)
    {
        fn(arg);
        ready.store(true, std::memory_order_release);
        done.release();
    }

    void get()
    {
        done.acquire();
    }
};

} // namespace detail

// Handle to an async result. v1: get() once; continuations come later.
template<typename R>
class Task
{
public:
    Task() = default;

    explicit Task(std::shared_ptr<detail::Task_state<R>> state) noexcept
        : state_(std::move(state))
    {}

    bool is_ready() const noexcept
    {
        return state_ && state_->ready.load(std::memory_order_acquire);
    }

    // Blocks until the task completes and returns its result. Call once.
    R get()
    {
        return state_->get();
    }

private:
    std::shared_ptr<detail::Task_state<R>> state_;
};

// The only sanctioned way to touch a T across threads. You never receive a bare
// T&; you hand a functor to async() and it runs once access has been granted.
// Access mode is deduced from the functor's parameter const-ness:
//   functor(T&)        -> read_write
//   functor(const T&)  -> read_only
template<typename T>
class Thread_safe
{
public:
    template<typename... Args>
    explicit Thread_safe(Args&&... args)
        : instance_(std::forward<Args>(args)...)
    {}

    // Identity matters (it is the access key); waits out pending jobs so the
    // pipe outlives its last task.
    ~Thread_safe()
    {
        pipe_.wait_until_idle();
    }

    Thread_safe(const Thread_safe&) = delete;
    Thread_safe& operator=(const Thread_safe&) = delete;

    // read_write: functor takes T& (and not const T&)
    template<typename Fn>
        requires std::invocable<Fn, T&> && (!std::invocable<Fn, const T&>)
    auto async(Fn&& fn) -> Task<std::invoke_result_t<Fn, T&>>
    {
        return launch<std::invoke_result_t<Fn, T&>, Access::read_write>(
            &instance_, std::forward<Fn>(fn));
    }

    // read_only: functor takes const T&
    template<typename Fn>
        requires std::invocable<Fn, const T&>
    auto async(Fn&& fn) const -> Task<std::invoke_result_t<Fn, const T&>>
    {
        return launch<std::invoke_result_t<Fn, const T&>, Access::read_only>(
            &instance_, std::forward<Fn>(fn));
    }

private:
    template<typename R, Access mode, typename Inst, typename Fn>
    Task<R> launch(Inst* inst, Fn&& fn) const
    {
        auto state = std::make_shared<detail::Task_state<R>>();

        detail::pipe_enqueue(default_scheduler(), pipe_, mode,
            [inst, state, fn = std::forward<Fn>(fn)]() mutable
            {
                Access_context ctx;
                ctx.add(inst, mode);
                Access_scope scope(ctx);
                state->run(fn, *inst);
            });

        return Task<R>(std::move(state));
    }

    T instance_;
    mutable detail::Pipe pipe_;
};

} // namespace ts
