#pragma once

#include "access.h"
#include "scheduler.h"
#include "task.h"

#include <concepts>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

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

// Acquire a pipe for out-of-band direct access in `mode`, holding it (not auto-completing)
// until `pipe_release`. A `Static_task_graph` node accesses its objects directly, bypassing
// the pipe, so it holds the pipe to keep async from racing that access -- but MODE-AWARE:
// a read_only holder joins concurrent readers (so two reader nodes, or a reader node and an
// async reader, overlap), a read_write holder is exclusive. Returns true if acquired now
// (admissible at the front, per the reader/writer rules); false if deferred, in which case
// `on_acquired` runs once the pipe drains to it (FIFO). Generalizes the old writer-only
// `pipe_reserve`; async coexistence is now per-node, not whole-run (see docs §10).
bool pipe_acquire(Scheduler& scheduler, Pipe& pipe, Access mode, std::move_only_function<void()> on_acquired);

// Release a hold taken by `pipe_acquire` in `mode`; admits queued jobs.
void pipe_release(Scheduler& scheduler, Pipe& pipe, Access mode);

// Extracts the parameter type list of a callable's `operator()` (or a function pointer).
// Non-generic lambdas / functors / function pointers only; generic `auto&` params aren't
// introspectable. Shared by `Static_task_graph::add_node` and multi-object `ts::async` for
// per-argument access-mode deduction.
template<typename T>
struct Function_traits : Function_traits<decltype(&T::operator())> {};
template<typename C, typename R, typename... A>
struct Function_traits<R(C::*)(A...)> { using args = std::tuple<A...>; };
template<typename C, typename R, typename... A>
struct Function_traits<R(C::*)(A...) const> { using args = std::tuple<A...>; };
template<typename R, typename... A>
struct Function_traits<R(*)(A...)> { using args = std::tuple<A...>; };

// `read_only` for a `const T&` parameter, `read_write` otherwise.
template<typename Arg>
constexpr Access async_mode_of()
{
    return std::is_const_v<std::remove_reference_t<Arg>> ? Access::read_only : Access::read_write;
}

// The pipes a multi-object async acquired (canonical / pipe-address order, deduped
// write-wins), released at the task's completion.
struct Multi_async_state
{
    Scheduler* scheduler = nullptr;
    std::vector<std::pair<Pipe*, Access>> holds;
};

// Acquire `state->holds[pos..]` in order (each held via `pipe_acquire`, mode-aware); once all
// are held, dispatch `block`. Immediate acquisitions recurse; a contended one defers to the
// callback. Canonical (pipe-address) order makes the multi-object acquire deadlock-free --
// the same order the graph uses (`distinct_pipes_` is address-sorted), so nodes and
// multi-object asyncs can't deadlock against each other.
void multi_acquire(std::shared_ptr<Multi_async_state> state,
                   Task_ptr block, std::size_t pos);

// Try to run an async job INLINE on the calling thread instead of enqueuing it (opt-in via
// `Task_options::run_inline`). Admissible only when the pipe is immediately free for this
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

// `Thread_safe::async` and the multi-object `ts::async` take `Task_options` (defined in
// task.h) — the same aggregate as `then`: `{token, priority, run_inline}`. `run_inline` runs
// the body synchronously on the calling thread when the pipe is immediately free (else it
// enqueues as usual) -- see the note on `async`.

// The only sanctioned way to touch a `T` across threads. You never receive a bare
// `T&`; you hand a functor to `async()` and it runs once access has been granted.
// Access mode is deduced from the functor's parameter const-ness:
//   `functor(T&)`       -> `read_write`
//   `functor(const T&)` -> `read_only`
class Static_task_graph;

namespace detail
{
// Grants the multi-object `ts::async` builder access to a `Thread_safe`'s instance + pipe
// (the same internals `Static_task_graph` reaches as a friend). Defined below `Thread_safe`.
struct Thread_safe_access;
}

template<typename T>
class Thread_safe
{
    friend class Static_task_graph;
    friend struct detail::Thread_safe_access;

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
    auto async(Fn&& fn, Task_options opts = {})
        -> Task<detail::Async_result_t<Fn, T&>>
    {
        return launch<detail::Async_result_t<Fn, T&>, Access::read_write>(
            &instance_, std::forward<Fn>(fn), opts);
    }

    // `read_only`: functor takes `const T&`, optionally + a trailing token
    template<typename Fn>
        requires detail::Async_accessor<Fn, const T&>
    auto async(Fn&& fn, Task_options opts = {}) const
        -> Task<detail::Async_result_t<Fn, const T&>>
    {
        return launch<detail::Async_result_t<Fn, const T&>, Access::read_only>(
            &instance_, std::forward<Fn>(fn), opts);
    }

private:
    template<typename R, Access mode, typename Inst, typename Fn>
    Task<R> launch(Inst* inst, Fn&& fn, Task_options opts) const
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

namespace detail
{

struct Thread_safe_access
{
    template<typename T> static T* instance(Thread_safe<T>& t) { return &t.instance_; }
    template<typename T> static Pipe& pipe(Thread_safe<T>& t) { return t.pipe_; }
};

// Build a multi-object async task: `fn(*objs...)` under an `Access_context` declaring every
// object (per-arg mode from `Args`), gated on holding all their pipes. Acquires the pipes in
// canonical order (deduped write-wins), releases them at completion via an attached
// continuation. Returns the `Task<R>`.
template<typename Args, std::size_t... I, typename Fn, typename... Ts>
auto async_build(Task_options opts, std::index_sequence<I...>, Fn&& fn, Thread_safe<Ts>&... objs)
{
    using R = std::invoke_result_t<Fn, Ts&...>;
    auto instances = std::make_tuple(Thread_safe_access::instance(objs)...);

    auto body = [instances, fn = std::forward<Fn>(fn)]() mutable -> R
    {
        Access_context ctx;
        (ctx.add(static_cast<const void*>(std::get<I>(instances)),
                 async_mode_of<std::tuple_element_t<I, Args>>()), ...);
        Access_scope scope(ctx);
        return fn(*std::get<I>(instances)...);
    };
    auto block = make_executable<R>(std::move(body), std::move(opts.token));
    block->flags.priority = opts.priority;

    // Collect (pipe, mode) per object, dedup write-wins; a `std::map` keyed by pipe address
    // gives the canonical (ascending-address) acquire order for free.
    Pipe* pipes[] = { &Thread_safe_access::pipe(objs)... };
    Access modes[] = { async_mode_of<std::tuple_element_t<I, Args>>()... };
    std::map<Pipe*, Access> by_pipe;
    for (std::size_t k = 0; k < sizeof...(Ts); ++k)
    {
        auto [it, inserted] = by_pipe.try_emplace(pipes[k], modes[k]);
        if (!inserted && modes[k] == Access::read_write)
            it->second = Access::read_write;
    }

    auto state = std::make_shared<Multi_async_state>();
    state->scheduler = &default_scheduler();
    for (const auto& [p, m] : by_pipe)
        state->holds.push_back({ p, m });

    // Release every held pipe once the body (and any nested sub-work) completes.
    block->attach([state](void*, bool)
    {
        for (const auto& [p, m] : state->holds)
            pipe_release(*state->scheduler, *p, m);
    });

    Task<R> result(block);
    multi_acquire(std::move(state), std::move(block), 0);
    return result;
}

} // namespace detail

// Multi-object async: run `fn(*obj1, *obj2, ...)` once it holds all the objects, with per-arg
// access deduced from the functor's parameter const-ness (`Function_traits`; non-generic
// lambdas / function pointers only). Deadlock-free (objects acquired in canonical order).
// Options come FIRST (a function parameter pack can't be followed by a defaulted arg); the
// no-options overload defaults them. `run_inline` on the options is ignored here (multi-object
// inline is a follow-up); `token`/`priority` apply as usual. Fire-and-forget or consume the
// `Task<R>` -- but do NOT block a graph node on it (same rule as single-object async).
template<typename Fn, typename... Ts>
    requires (sizeof...(Ts) >= 1)
auto async(Task_options opts, Fn&& fn, Thread_safe<Ts>&... objs)
{
    using Args = typename detail::Function_traits<std::decay_t<Fn>>::args;
    static_assert(std::tuple_size_v<Args> == sizeof...(Ts),
        "multi-object async: functor arity must match the number of Thread_safe objects");
    return detail::async_build<Args>(std::move(opts), std::index_sequence_for<Ts...>{},
        std::forward<Fn>(fn), objs...);
}

template<typename Fn, typename... Ts>
    requires (sizeof...(Ts) >= 1)
auto async(Fn&& fn, Thread_safe<Ts>&... objs)
{
    return async(Task_options{}, std::forward<Fn>(fn), objs...);
}

// `ts::launch` / `ts::nested` (bare scheduler tasks) live in task.h now — they dispatch
// through the `submit_ready` bridge and need no pipe, so they belong with the task core.

} // namespace ts
