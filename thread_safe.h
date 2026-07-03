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
void submit_closure(Scheduler& scheduler, std::move_only_function<void()> closure,
                    Priority priority = Priority::normal);

struct Job
{
    Access mode;
    std::move_only_function<void()> fn;
    bool reservation = false;   // if set, `fn` is an on-acquired callback; see `pipe_reserve`
    Priority priority = Priority::normal;   // queue position when this job is admitted
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

void pipe_enqueue(Scheduler& scheduler, Pipe& pipe, Access mode, std::move_only_function<void()> fn,
                  Priority priority = Priority::normal);

// Reserve a pipe for exclusive out-of-band use (a `Static_task_graph` run accesses
// its objects directly, bypassing the pipe, so it must hold the pipe to keep async
// jobs from racing that direct access). Behaves as an exclusive (writer) holder that
// does not auto-complete: async jobs queue behind it until `pipe_release`.
// Returns true if acquired synchronously (pipe was idle); false if deferred, in which
// case `on_acquired` runs once the pipe drains to the reservation.
bool pipe_reserve(Scheduler& scheduler, Pipe& pipe, std::move_only_function<void()> on_acquired);

// Release a reservation taken by `pipe_reserve`; admits queued jobs.
void pipe_release(Scheduler& scheduler, Pipe& pipe);

// Try to run an async job INLINE on the calling thread instead of enqueuing it (opt-in via
// `Async_options::run_inline`). Admissible only when the pipe is immediately free for this
// mode -- no queued jobs (FIFO preserved) and the reader/writer rules allow: `read_only`
// joins as a concurrent reader, `read_write` as an exclusive writer. On success runs `fn()`
// synchronously (the caller blocks for the body's duration), then releases, re-dispatches
// the pipe, and returns true. On failure leaves `fn` untouched (so the caller can enqueue
// it) and returns false. Caller-blocking + a nested access scope -- see `Thread_safe::async`.
bool pipe_try_inline(Scheduler& scheduler, Pipe& pipe, Access mode, std::move_only_function<void()>& fn);

// An `async` accessor functor may, like the bare-task path, opt into cooperative
// cancellation by taking a trailing `Cancellation_token` after the access argument `A`
// (`[](T& v, Cancellation_token t){...}`). These accept either arity for a given `A`, so
// the read/write disambiguation and result deduction work whether or not the token is
// declared. `Executable::run` forwards the block's token to the token-taking body.
template<typename Fn, typename A>
concept Async_accessor = std::invocable<Fn, A> || std::invocable<Fn, A, const Cancellation_token&>;

template<typename Fn, typename A>
inline constexpr bool accessor_takes_token_v = std::invocable<Fn, A, const Cancellation_token&>;

template<typename Fn, typename A, bool = accessor_takes_token_v<Fn, A>>
struct Async_result { using type = std::invoke_result_t<Fn, A, const Cancellation_token&>; };
template<typename Fn, typename A>
struct Async_result<Fn, A, false> { using type = std::invoke_result_t<Fn, A>; };
template<typename Fn, typename A> using Async_result_t = typename Async_result<Fn, A>::type;

} // namespace detail

// Dispatch options for `Thread_safe::async`. An aggregate, so it takes designated
// initializers at the call site: `obj.async(fn, {.priority = Priority::high})`,
// `obj.async(fn, {.run_inline = true})`. `token` makes the job skippable before it runs
// (and, if the body declares a trailing `Cancellation_token`, is forwarded to it for a
// mid-run early-out). `run_inline` runs the body synchronously on the calling thread when
// the pipe is immediately free (else it enqueues as usual) -- see the note on `async`.
// Deliberately a distinct struct from `Continuation_options` (async is not a continuation).
struct Async_options
{
    Cancellation_token token = {};
    Priority priority = Priority::normal;
    bool run_inline = false;
};

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

    // `read_write`: functor takes `T&` (and not `const T&`), optionally + a trailing token.
    // With `{.run_inline = true}` the body runs synchronously on the CALLING thread when the
    // pipe is free (see below); it then blocks the caller and stacks its access scope, so do
    // NOT opt in from a worker you can't afford to block (e.g. inside a graph node).
    template<typename Fn>
        requires detail::Async_accessor<Fn, T&> && (!detail::Async_accessor<Fn, const T&>)
    auto async(Fn&& fn, Async_options opts = {})
        -> Task<detail::Async_result_t<Fn, T&>>
    {
        return launch<detail::Async_result_t<Fn, T&>, Access::read_write>(
            &instance_, std::forward<Fn>(fn), opts);
    }

    // `read_only`: functor takes `const T&`, optionally + a trailing token
    template<typename Fn>
        requires detail::Async_accessor<Fn, const T&>
    auto async(Fn&& fn, Async_options opts = {}) const
        -> Task<detail::Async_result_t<Fn, const T&>>
    {
        return launch<detail::Async_result_t<Fn, const T&>, Access::read_only>(
            &instance_, std::forward<Fn>(fn), opts);
    }

private:
    template<typename R, Access mode, typename Inst, typename Fn>
    Task<R> launch(Inst* inst, Fn&& fn, Async_options opts) const
    {
        // The body (stored in the block) runs `fn` under this object's access scope. If
        // `fn` takes a trailing token, the body does too and `Executable::run` forwards the
        // block's token (uniform with the bare-task path's `with_inherited_access`).
        auto core = [&]
        {
            if constexpr (detail::accessor_takes_token_v<Fn, decltype(*inst)>)
            {
                auto body = [inst, fn = std::forward<Fn>(fn)](const Cancellation_token& tok) mutable -> R
                {
                    Access_context ctx;
                    ctx.add(inst, mode);
                    Access_scope scope(ctx);
                    return fn(*inst, tok);
                };
                return detail::make_executable<R>(std::move(body), opts.token);
            }
            else
            {
                auto body = [inst, fn = std::forward<Fn>(fn)]() mutable -> R
                {
                    Access_context ctx;
                    ctx.add(inst, mode);
                    Access_scope scope(ctx);
                    return fn(*inst);
                };
                return detail::make_executable<R>(std::move(body), opts.token);
            }
        }();
        core->flags.priority = opts.priority;

        std::move_only_function<void()> job = [core, gen = core->generation()] { core->execute(core, gen); };
        // Inline fast-path: if opted in and the pipe is free right now, run the body on this
        // thread; otherwise enqueue as usual (`pipe_try_inline` leaves `job` untouched).
        if (opts.run_inline && detail::pipe_try_inline(default_scheduler(), pipe_, mode, job))
            return Task<R>(core);
        detail::pipe_enqueue(default_scheduler(), pipe_, mode, std::move(job), opts.priority);
        return Task<R>(core);
    }

    T instance_;
    mutable detail::Pipe pipe_;
};

// `ts::launch` / `ts::nested` (bare scheduler tasks) live in task.h now — they dispatch
// through the `submit_ready` bridge and need no pipe, so they belong with the task core.

} // namespace ts
