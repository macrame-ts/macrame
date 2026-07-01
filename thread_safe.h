#pragma once

#include "access.h"
#include "scheduler.h"
#include "task.h"

#include <concepts>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace ts
{

// Ambient scheduler used by `async()` (v1: a process-wide default).
Scheduler& default_scheduler();

namespace detail
{

// Submit a closure to the scheduler (bridges to the raw func-ptr API).
void submit_closure(Scheduler& scheduler, std::move_only_function<void()> closure);

struct Job
{
    Access mode;
    std::move_only_function<void()> fn;
    bool reservation = false;   // if set, `fn` is an on-acquired callback; see `pipe_reserve`
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

// Reserve a pipe for exclusive out-of-band use (a `Static_task_graph` run accesses
// its objects directly, bypassing the pipe, so it must hold the pipe to keep async
// jobs from racing that direct access). Behaves as an exclusive (writer) holder that
// does not auto-complete: async jobs queue behind it until `pipe_release`.
// Returns true if acquired synchronously (pipe was idle); false if deferred, in which
// case `on_acquired` runs once the pipe drains to the reservation.
bool pipe_reserve(Scheduler& scheduler, Pipe& pipe, std::move_only_function<void()> on_acquired);

// Release a reservation taken by `pipe_reserve`; admits queued jobs.
void pipe_release(Scheduler& scheduler, Pipe& pipe);

} // namespace detail

// The only sanctioned way to touch a `T` across threads. You never receive a bare
// `T&`; you hand a functor to `async()` and it runs once access has been granted.
// Access mode is deduced from the functor's parameter const-ness:
//   `functor(T&)`       -> `read_write`
//   `functor(const T&)` -> `read_only`
class Static_task_graph;

template<typename T>
class Thread_safe
{
    friend class Static_task_graph;

public:
    // Non-explicit default ctor (value-initializes `T`) so arrays of `Thread_safe`
    // work; the forwarding ctor below handles explicit argument construction.
    Thread_safe() requires std::default_initializable<T>
        : instance_()
    {}

    // Constrained so it never shadows the (deleted) copy/move constructors:
    // 1+ args, and only when T is actually constructible from them.
    template<typename... Args>
        requires (sizeof...(Args) >= 1) && std::constructible_from<T, Args...>
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

    // `read_write`: functor takes `T&` (and not `const T&`)
    template<typename Fn>
        requires std::invocable<Fn, T&> && (!std::invocable<Fn, const T&>)
    auto async(Fn&& fn, Cancellation_token token = {}) -> Task<std::invoke_result_t<Fn, T&>>
    {
        return launch<std::invoke_result_t<Fn, T&>, Access::read_write>(
            &instance_, std::forward<Fn>(fn), token);
    }

    // `read_only`: functor takes `const T&`
    template<typename Fn>
        requires std::invocable<Fn, const T&>
    auto async(Fn&& fn, Cancellation_token token = {}) const -> Task<std::invoke_result_t<Fn, const T&>>
    {
        return launch<std::invoke_result_t<Fn, const T&>, Access::read_only>(
            &instance_, std::forward<Fn>(fn), token);
    }

private:
    template<typename R, Access mode, typename Inst, typename Fn>
    Task<R> launch(Inst* inst, Fn&& fn, Cancellation_token token) const
    {
        auto state = std::make_shared<detail::Task_control_block<R>>();

        detail::pipe_enqueue(default_scheduler(), pipe_, mode,
            [inst, state, fn = std::forward<Fn>(fn), token]() mutable
            {
                if (token.is_cancel_requested())   // skip the body; settle as cancelled
                {
                    state->cancel();
                    return;
                }
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
