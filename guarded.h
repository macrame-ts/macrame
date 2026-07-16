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

// A queued pipe job, in one of two flavors. A normal async job carries its task BLOCK as the
// payload (no closure -- the block is dispatched raw via a static trampoline, mirroring
// `submit_ready`, so admitting a job allocates nothing). A `reservation` carries the
// `pipe_acquire` on-acquired callback instead (the graph / multi-async path). The deque is
// mutex-guarded, so `Job` may be any size -- unlike the 16-byte lock-free `Task_entry`.
struct Job
{
    Access mode;
    bool reservation = false;                      // if set, `on_acquired` is the payload
    Priority priority = Priority::normal;          // queue position when this job is admitted
    Task_ptr block;                                // normal async job: the block IS the payload
    std::move_only_function<void()> on_acquired;   // reservation: signals the deferred holder
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

// Enqueue `block` as a pipe job in `mode`; when admitted (reader/writer rules, FIFO) it is
// dispatched to the scheduler as a raw block trampoline (`block->execute`, gen 0 -- pipe
// blocks are never reset) and the pipe is released when the body returns. Allocation-free.
void pipe_enqueue(Scheduler& scheduler, Pipe& pipe, Access mode, Task_ptr block,
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
struct Multi_async_state : Ref_counted<Multi_async_state>
{
    Scheduler* scheduler = nullptr;
    std::vector<std::pair<Pipe*, Access>> holds;
};

// Acquire `state->holds[pos..]` in order (each held via `pipe_acquire`, mode-aware); once all
// are held, dispatch `block`. Immediate acquisitions recurse; a contended one defers to the
// callback. Canonical (pipe-address) order makes the multi-object acquire deadlock-free --
// the same order the graph uses (`distinct_pipes_` is address-sorted), so nodes and
// multi-object asyncs can't deadlock against each other.
void multi_acquire(Ref_ptr<Multi_async_state> state,
                   Task_ptr block, std::size_t pos);

// Try to run an async job INLINE on the calling thread instead of enqueuing it (opt-in via
// `Task_options::run_inline`). Admissible only when the pipe is immediately free for this
// mode -- no queued jobs (FIFO preserved) and the reader/writer rules allow: `read_only`
// joins as a concurrent reader, `read_write` as an exclusive writer. On success runs the
// block's body synchronously (the caller blocks for its duration), then releases,
// re-dispatches the pipe, and returns true. On failure returns false and the caller
// enqueues the same block. Caller-blocking + a nested access scope -- see `Guarded::async`.
bool pipe_try_inline(Scheduler& scheduler, Pipe& pipe, Access mode, const Task_ptr& block);

// An `async` accessor functor may, like the bare-task path, opt into cooperative
// cancellation by taking a trailing `Cancellation_token` after the access argument `A`
// (`[](T& v, Cancellation_token t){...}`). These accept either arity for a given `A`, so
// the read/write disambiguation and result deduction work whether or not the token is
// declared. `Executable::run` forwards the block's token to the token-taking body.
template<typename Fn, typename A>
concept Async_accessor = std::invocable<Fn, A> || std::invocable<Fn, A, const Cancellation_token&>;

template<typename Fn, typename A>
inline constexpr bool accessor_takes_token_v = std::invocable<Fn, A, const Cancellation_token&>;

// Result type of an accessor, picking the token-taking overload when present. Guarded by
// an outer `Invocable` layer: forming `Async_result_t` for a NON-invocable (Fn, A) yields a
// benign `void` instead of hard-instantiating `invoke_result_t` on a bad combination. MSVC
// evaluates a *rejected* overload's trailing return type (the other access mode's
// `Async_result_t<Fn, const T&>` for a write lambda) during overload resolution, where clang
// SFINAEs it away; without this guard MSVC hard-errors (C2794/C2938).
template<typename Fn, typename A, bool = accessor_takes_token_v<Fn, A>>
struct Async_result_sel { using type = std::invoke_result_t<Fn, A, const Cancellation_token&>; };
template<typename Fn, typename A>
struct Async_result_sel<Fn, A, false> { using type = std::invoke_result_t<Fn, A>; };

template<typename Fn, typename A,
         bool = std::invocable<Fn, A> || accessor_takes_token_v<Fn, A>>
struct Async_result { using type = void; };
template<typename Fn, typename A>
struct Async_result<Fn, A, true> : Async_result_sel<Fn, A> {};

template<typename Fn, typename A> using Async_result_t = typename Async_result<Fn, A>::type;

} // namespace detail

// `Guarded::access`/`async` and the multi-object `ts::access`/`ts::async` take `Task_options`
// (defined in task.h) for `{token, priority}` (the `run_inline` field of that shared aggregate
// is used by `then`, not by these -- the verb chooses inline vs enqueued).

// The only sanctioned way to touch a `T` across threads. You never receive a bare `T&`; you
// hand a functor to `access()` (opportunistic -- inline when free) or `async()` (always
// enqueued) and it runs once access has been granted. Access mode is deduced from the functor's
// parameter const-ness:
//   `functor(T&)`       -> `read_write`
//   `functor(const T&)` -> `read_only`
class Static_task_graph;

namespace detail
{
// Grants the multi-object `ts::access`/`ts::async` builder access to a `Guarded`'s instance + pipe
// (the same internals `Static_task_graph` reaches as a friend). Defined below `Guarded`.
struct Guarded_access;
}

template<typename T>
class Guarded
{
    friend class Static_task_graph;
    friend struct detail::Guarded_access;

public:
    // Non-explicit default ctor (value-initializes `T`) so arrays of `Guarded`
    // work; the forwarding ctor below handles explicit argument construction.
    Guarded() requires std::default_initializable<T>
        : instance_()
    {}

    // Constrained so it never shadows the (deleted) copy/move constructors:
    // 1+ args, and only when T is actually constructible from them.
    template<typename... Args>
        requires (sizeof...(Args) >= 1) && std::constructible_from<T, Args...>
    explicit Guarded(Args&&... args)
        : instance_(std::forward<Args>(args)...)
    {}

    // Identity matters (it is the access key); waits out pending jobs so the
    // pipe outlives its last task.
    ~Guarded()
    {
        pipe_.wait_until_idle();
    }

    Guarded(const Guarded&) = delete;
    Guarded& operator=(const Guarded&) = delete;

    // Two verbs run a functor under this object's access. Both deduce the mode from the
    // functor's parameter const-ness -- `T&` = read_write (exclusive), `const T&` = read_only
    // (concurrent readers) -- accept a trailing `Cancellation_token` accessor, and take
    // `Task_options` for `{token, priority}`. They do NOT read `run_inline`: the verb IS the mode.
    //
    //   access(fn) -- opportunistic: runs `fn` on the CALLING thread when the pipe is free right
    //                 now (no scheduling), otherwise enqueues. Best for short functors. Because it
    //                 may run inline it can briefly block the caller and stacks its access scope,
    //                 so prefer `async` for anything non-trivial inside a graph node.
    //   async(fn)  -- always enqueued off the calling thread. For heavy functors.

    // access, read_write: functor takes `T&` (and not `const T&`), optionally + a trailing token.
    template<typename Fn>
        requires detail::Async_accessor<Fn, T&> && (!detail::Async_accessor<Fn, const T&>)
    auto access(Fn&& fn, Task_options opts = {})
        -> Task<detail::Async_result_t<Fn, T&>>
    {
        return launch<detail::Async_result_t<Fn, T&>, Access::read_write>(
            &instance_, std::forward<Fn>(fn), opts, /*try_inline=*/true);
    }

    // access, read_only: functor takes `const T&`, optionally + a trailing token.
    template<typename Fn>
        requires detail::Async_accessor<Fn, const T&>
    auto access(Fn&& fn, Task_options opts = {}) const
        -> Task<detail::Async_result_t<Fn, const T&>>
    {
        return launch<detail::Async_result_t<Fn, const T&>, Access::read_only>(
            &instance_, std::forward<Fn>(fn), opts, /*try_inline=*/true);
    }

    // async, read_write: always enqueued (never inline).
    template<typename Fn>
        requires detail::Async_accessor<Fn, T&> && (!detail::Async_accessor<Fn, const T&>)
    auto async(Fn&& fn, Task_options opts = {})
        -> Task<detail::Async_result_t<Fn, T&>>
    {
        return launch<detail::Async_result_t<Fn, T&>, Access::read_write>(
            &instance_, std::forward<Fn>(fn), opts, /*try_inline=*/false);
    }

    // async, read_only: always enqueued (never inline).
    template<typename Fn>
        requires detail::Async_accessor<Fn, const T&>
    auto async(Fn&& fn, Task_options opts = {}) const
        -> Task<detail::Async_result_t<Fn, const T&>>
    {
        return launch<detail::Async_result_t<Fn, const T&>, Access::read_only>(
            &instance_, std::forward<Fn>(fn), opts, /*try_inline=*/false);
    }

private:
    template<typename R, Access mode, typename Inst, typename Fn>
    Task<R> launch(Inst* inst, Fn&& fn, Task_options opts, bool try_inline) const
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

        // The block IS the pipe job -- no closure. Inline fast-path (`access`): if `try_inline`
        // and the pipe is free right now, run the body on this thread; otherwise (or for `async`)
        // enqueue as usual.
        if (try_inline && detail::pipe_try_inline(default_scheduler(), pipe_, mode, core))
            return Task<R>(core);
        detail::pipe_enqueue(default_scheduler(), pipe_, mode, core, opts.priority);
        return Task<R>(core);
    }

    T instance_;
    mutable detail::Pipe pipe_;
};

namespace detail
{

struct Guarded_access
{
    template<typename T> static T* instance(Guarded<T>& t) { return &t.instance_; }
    template<typename T> static Pipe& pipe(Guarded<T>& t) { return t.pipe_; }
};

// Build a multi-object async task: `fn(*objs...)` under an `Access_context` declaring every
// object (per-arg mode from `Args`), gated on holding all their pipes. Acquires the pipes in
// canonical order (deduped write-wins), releases them at completion via an attached
// continuation. Returns the `Task<R>`.
template<typename Args, std::size_t... I, typename Fn, typename... Ts>
auto async_build(Task_options opts, std::index_sequence<I...>, Fn&& fn, Guarded<Ts>&... objs)
{
    using R = std::invoke_result_t<Fn, Ts&...>;
    auto instances = std::make_tuple(Guarded_access::instance(objs)...);

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
    Pipe* pipes[] = { &Guarded_access::pipe(objs)... };
    Access modes[] = { async_mode_of<std::tuple_element_t<I, Args>>()... };
    std::map<Pipe*, Access> by_pipe;
    for (std::size_t k = 0; k < sizeof...(Ts); ++k)
    {
        auto [it, inserted] = by_pipe.try_emplace(pipes[k], modes[k]);
        if (!inserted && modes[k] == Access::read_write)
            it->second = Access::read_write;
    }

    auto state = make_ref<Multi_async_state>();
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
auto async(Task_options opts, Fn&& fn, Guarded<Ts>&... objs)
{
    using Args = typename detail::Function_traits<std::decay_t<Fn>>::args;
    static_assert(std::tuple_size_v<Args> == sizeof...(Ts),
        "multi-object async: functor arity must match the number of Guarded objects");
    return detail::async_build<Args>(std::move(opts), std::index_sequence_for<Ts...>{},
        std::forward<Fn>(fn), objs...);
}

template<typename Fn, typename... Ts>
    requires (sizeof...(Ts) >= 1)
auto async(Fn&& fn, Guarded<Ts>&... objs)
{
    return async(Task_options{}, std::forward<Fn>(fn), objs...);
}

// Multi-object `access`: the opportunistic sibling of `ts::async(fn, objs...)`. The multi-object
// inline fast path is a follow-up, so for now `access` here behaves exactly like `async`.
template<typename Fn, typename... Ts>
    requires (sizeof...(Ts) >= 1)
auto access(Task_options opts, Fn&& fn, Guarded<Ts>&... objs)
{
    return async(std::move(opts), std::forward<Fn>(fn), objs...);
}

template<typename Fn, typename... Ts>
    requires (sizeof...(Ts) >= 1)
auto access(Fn&& fn, Guarded<Ts>&... objs)
{
    return async(std::forward<Fn>(fn), objs...);
}

// `ts::launch` / `ts::nested` (bare scheduler tasks) live in task.h now — they dispatch
// through the `submit_ready` bridge and need no pipe, so they belong with the task core.

} // namespace ts
