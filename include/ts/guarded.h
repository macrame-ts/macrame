#pragma once

#include "ts/access.h"
#include "ts/scheduler.h"
#include "ts/task.h"

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

// Ambient scheduler used by `access()` / `async()` (v1: a process-wide default).
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

// Try to run a job INLINE on the calling thread instead of enqueuing it (the `access` verb's
// fast path). Admissible only when the pipe is immediately free for this mode -- no queued jobs
// (FIFO preserved) and the reader/writer rules allow: `read_only`
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

// `Guarded::access`/`async` and the multi-object `ts::access`/`ts::async` take `Access_options`
// (defined in task.h) = `{token, priority}`. There is deliberately no `run_inline` field -- the
// verb chooses inline vs enqueued (`access` inline-when-free, `async` always enqueued), so the
// impossible option can't be passed. (`then`/task builders use `Task_options`, which has it.)

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
    // `Access_options` = `{token, priority}` (no `run_inline`: the verb IS the mode).
    //
    //   access(fn) -- opportunistic: runs `fn` on the CALLING thread when the pipe is free right
    //                 now (no scheduling), otherwise enqueues. Best for short functors. Because it
    //                 may run inline it can briefly block the caller and stacks its access scope,
    //                 so prefer `async` for anything non-trivial inside a graph node.
    //   async(fn)  -- always enqueued off the calling thread. For heavy functors.

    // access, read_write: functor takes `T&` (and not `const T&`), optionally + a trailing token.
    template<typename Fn>
        requires detail::Async_accessor<Fn, T&> && (!detail::Async_accessor<Fn, const T&>)
    auto access(Fn&& fn, Access_options opts = {})
        -> Task<detail::Async_result_t<Fn, T&>>
    {
        return launch<detail::Async_result_t<Fn, T&>, Access::read_write>(
            &instance_, std::forward<Fn>(fn), opts, /*try_inline=*/true);
    }

    // access, read_only: functor takes `const T&`, optionally + a trailing token.
    template<typename Fn>
        requires detail::Async_accessor<Fn, const T&>
    auto access(Fn&& fn, Access_options opts = {}) const
        -> Task<detail::Async_result_t<Fn, const T&>>
    {
        return launch<detail::Async_result_t<Fn, const T&>, Access::read_only>(
            &instance_, std::forward<Fn>(fn), opts, /*try_inline=*/true);
    }

    // async, read_write: always enqueued (never inline).
    template<typename Fn>
        requires detail::Async_accessor<Fn, T&> && (!detail::Async_accessor<Fn, const T&>)
    auto async(Fn&& fn, Access_options opts = {})
        -> Task<detail::Async_result_t<Fn, T&>>
    {
        return launch<detail::Async_result_t<Fn, T&>, Access::read_write>(
            &instance_, std::forward<Fn>(fn), opts, /*try_inline=*/false);
    }

    // async, read_only: always enqueued (never inline).
    template<typename Fn>
        requires detail::Async_accessor<Fn, const T&>
    auto async(Fn&& fn, Access_options opts = {}) const
        -> Task<detail::Async_result_t<Fn, const T&>>
    {
        return launch<detail::Async_result_t<Fn, const T&>, Access::read_only>(
            &instance_, std::forward<Fn>(fn), opts, /*try_inline=*/false);
    }

private:
    template<typename R, Access mode, typename Inst, typename Fn>
    Task<R> launch(Inst* inst, Fn&& fn, Access_options opts, bool try_inline) const
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

// An access-mode-tagged object argument, produced by `ts::as_read_only(g)` / `ts::as_read_write(g)`. It
// lets a GENERIC lambda (`[](auto& x){...}`) declare per-object access explicitly: a generic
// lambda's `operator()` is a template with no introspectable parameter const-ness, so
// `Function_traits` cannot deduce read-vs-write -- the tag supplies it. The mode is a compile-time
// template argument, so everything downstream (the `Access_context`, edge derivation, acquire) is
// identical to the deduced path. Used by the multi-object `ts::access`/`ts::async` and
// `Static_task_graph::add_node`.
template<typename T, Access M>
struct Access_arg
{
    using value_type = T;
    static constexpr Access mode = M;
    Guarded<T>* obj;
};

template<typename A> struct is_access_arg : std::false_type {};
template<typename T, Access M> struct is_access_arg<Access_arg<T, M>> : std::true_type {};
template<typename A> inline constexpr bool is_access_arg_v = is_access_arg<std::remove_cvref_t<A>>::value;

template<typename A> struct is_guarded : std::false_type {};
template<typename T> struct is_guarded<Guarded<T>> : std::true_type {};
template<typename A> inline constexpr bool is_guarded_v = is_guarded<std::remove_cvref_t<A>>::value;

// A valid object argument to `add_node` / multi-object `ts::access`/`ts::async`: either a bare
// `Guarded<T>&` (mode deduced from the functor's parameter const-ness) or an `Access_arg<T, M>`
// from `ts::as_read_only`/`as_read_write` (mode explicit; for generic lambdas). Per call the two kinds must
// not be mixed -- see the `static_assert` at each entry point.
template<typename A>
concept Object_arg = is_guarded_v<A> || is_access_arg_v<A>;

// Shared tail for the multi-object builders: given the already-built `block` and this call's
// per-object (pipe, mode) arrays, dedup write-wins into the canonical (ascending pipe-address)
// acquire order, attach the release, and kick off `multi_acquire`. Identical for the deduced and
// tagged paths -- only the body/mode source differs, above this.
template<typename R>
Task<R> async_dispatch(Task_ptr block, Pipe* const* pipes, const Access* modes, std::size_t n)
{
    std::map<Pipe*, Access> by_pipe;
    for (std::size_t k = 0; k < n; ++k)
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

// Build a multi-object async task: `fn(*objs...)` under an `Access_context` declaring every
// object (per-arg mode DEDUCED from `Args`, the functor's parameter types), gated on holding all
// their pipes. The `access`/`async` entry points use this for bare `Guarded<T>&` arguments.
template<typename Args, std::size_t... I, typename Fn, typename... Ts>
auto async_build(Access_options opts, std::index_sequence<I...>, Fn&& fn, Guarded<Ts>&... objs)
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

    Pipe* pipes[] = { &Guarded_access::pipe(objs)... };
    Access modes[] = { async_mode_of<std::tuple_element_t<I, Args>>()... };
    return async_dispatch<R>(std::move(block), pipes, modes, sizeof...(Ts));
}

// The tagged sibling of `async_build`: every argument is an `Access_arg<T, M>` (from
// `ts::as_read_only`/`as_read_write`), so the per-object mode is the tag's `M` -- no `Function_traits`,
// which lets the functor be a generic lambda. Otherwise identical (same body shape, same
// dispatch tail).
template<std::size_t... I, typename Fn, typename... Objs>
auto async_build_tagged(Access_options opts, std::index_sequence<I...>, Fn&& fn, Objs&&... objs)
{
    using R = std::invoke_result_t<Fn, typename std::remove_cvref_t<Objs>::value_type&...>;
    auto instances = std::make_tuple(Guarded_access::instance(*objs.obj)...);

    auto body = [instances, fn = std::forward<Fn>(fn)]() mutable -> R
    {
        Access_context ctx;
        (ctx.add(static_cast<const void*>(std::get<I>(instances)),
                 std::remove_cvref_t<Objs>::mode), ...);
        Access_scope scope(ctx);
        return fn(*std::get<I>(instances)...);
    };
    auto block = make_executable<R>(std::move(body), std::move(opts.token));
    block->flags.priority = opts.priority;

    Pipe* pipes[] = { &Guarded_access::pipe(*objs.obj)... };
    Access modes[] = { std::remove_cvref_t<Objs>::mode... };
    return async_dispatch<R>(std::move(block), pipes, modes, sizeof...(Objs));
}

} // namespace detail

// Tag an object argument with an explicit access mode, for use with a GENERIC lambda (whose
// parameter const-ness can't be introspected): `graph.add_node([](auto& p, auto& n){ n.q(p); },
// ts::as_read_write(physics), ts::as_read_only(nav))`, and likewise `ts::access`/`ts::async`. With a
// NON-generic lambda you don't need these -- the mode is deduced from the parameter const-ness.
// (Named `as_read_only`/`as_read_write` to avoid colliding with the coroutine pipe guards
// `ts::read_only`/`ts::read_write` in coroutine_support.h.)
template<typename T>
detail::Access_arg<T, Access::read_only> as_read_only(Guarded<T>& g) { return { &g }; }
template<typename T>
detail::Access_arg<T, Access::read_write> as_read_write(Guarded<T>& g) { return { &g }; }

// Multi-object async: run `fn(*obj1, *obj2, ...)` once it holds all the objects. Per-object
// access is deduced from the functor's parameter const-ness for bare `Guarded<T>&` arguments
// (`Function_traits`; non-generic lambdas / function pointers), OR taken from an explicit
// `ts::as_read_only`/`as_read_write` tag on every argument (which lets the functor be a GENERIC lambda).
// The two kinds must not be mixed in one call. Deadlock-free (objects acquired in canonical
// order). Options come FIRST (a function parameter pack can't be followed by a defaulted arg);
// the no-options overload defaults them. `token`/`priority` apply as usual. Fire-and-forget or
// consume the `Task<R>` -- but do NOT block a graph node on it (same rule as single-object async).
template<typename Fn, typename... Objs>
    requires (sizeof...(Objs) >= 1) && (detail::Object_arg<Objs> && ...)
auto async(Access_options opts, Fn&& fn, Objs&&... objs)
{
    constexpr bool any_tagged = (detail::is_access_arg_v<Objs> || ...);
    if constexpr (any_tagged)
    {
        static_assert((detail::is_access_arg_v<Objs> && ...),
            "multi-object async: don't mix tagged (ts::as_read_only/as_read_write) and bare Guarded "
            "arguments -- tag EVERY object argument, or tag none");
        return detail::async_build_tagged(std::move(opts), std::index_sequence_for<Objs...>{},
            std::forward<Fn>(fn), std::forward<Objs>(objs)...);
    }
    else
    {
        using Args = typename detail::Function_traits<std::decay_t<Fn>>::args;
        static_assert(std::tuple_size_v<Args> == sizeof...(Objs),
            "multi-object async: functor arity must match the number of Guarded objects");
        return detail::async_build<Args>(std::move(opts), std::index_sequence_for<Objs...>{},
            std::forward<Fn>(fn), objs...);
    }
}

template<typename Fn, typename... Objs>
    requires (sizeof...(Objs) >= 1) && (detail::Object_arg<Objs> && ...)
auto async(Fn&& fn, Objs&&... objs)
{
    return async(Access_options{}, std::forward<Fn>(fn), std::forward<Objs>(objs)...);
}

// Multi-object `access`: the opportunistic sibling of `ts::async(fn, objs...)`. NOTE: the
// multi-object inline fast path is unimplemented, so `access` here currently behaves exactly
// like `async` (always enqueued) -- unlike single-object `access`, which runs inline when the
// queue is free. Tracked in docs/TODO.md (Guarded/access). Documented so the difference is not
// a silent surprise. Accepts the same bare-or-tagged arguments as `ts::async`.
template<typename Fn, typename... Objs>
    requires (sizeof...(Objs) >= 1) && (detail::Object_arg<Objs> && ...)
auto access(Access_options opts, Fn&& fn, Objs&&... objs)
{
    return async(std::move(opts), std::forward<Fn>(fn), std::forward<Objs>(objs)...);
}

template<typename Fn, typename... Objs>
    requires (sizeof...(Objs) >= 1) && (detail::Object_arg<Objs> && ...)
auto access(Fn&& fn, Objs&&... objs)
{
    return async(std::forward<Fn>(fn), std::forward<Objs>(objs)...);
}

// `ts::launch` / `ts::nested` (bare scheduler tasks) live in task.h now — they dispatch
// through the `submit_ready` bridge and need no pipe, so they belong with the task core.

} // namespace ts
