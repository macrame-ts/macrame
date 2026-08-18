#pragma once

// The `Guarded<T>` access-controlled wrapper and the free `ts::access`/`ts::async` verbs over it -
// the sanctioned way to share a thread-unsafe `T` across threads: hand the library a functor
// instead of locking `T` by hand, and it runs under serialized access (concurrent reads, one
// exclusive write). The full model is documented on `class Guarded<T>` below. The per-object
// serializer it rides lives in `detail/pipe.h`, the compile-time access-mode deduction in
// `detail/access_deduction.h`. User guide: docs/guide.md §5; serializer internals and the evolved
// cascade: docs/pipe-rebase.md §0.

#include "ts/access.h"
#include "ts/detail/access_deduction.h"
#include "ts/detail/pipe.h"
#include "ts/scheduler.h"
#include "ts/task.h"

#include <concepts>
#include <cstdio>
#include <functional>
#include <memory>
#include <source_location>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ts
{

// Ambient scheduler used by `access()` / `async()` (v1: a process-wide default).
Scheduler& global_scheduler();

namespace detail
{

// Submit a closure to the scheduler (bridges to the raw func-ptr API).
void submit_closure(Scheduler& scheduler, std::move_only_function<void()> closure,
                    Priority priority = Priority::normal);

// `submit_ready` (task.h), but to an explicitly resolved scheduler rather than
// `global_scheduler()`. The graph run caches its scheduler once (`Run_state::scheduler`)
// and dispatches every node through this, so the per-node dispatch skips the `g_fast`
// re-resolve (Opt 1 in docs/graph-regression-callgrind.md §4).
void submit_ready_on(Scheduler& scheduler, Task_ptr block);

// `submit_borrowed` (task.h) to an explicitly resolved scheduler - the cached-scheduler
// borrowed dispatch a graph run uses for its object-free nodes (Opt 1 + Opt 2).
void submit_borrowed_on(Scheduler& scheduler, Task_control_block* blk);

} // namespace detail

class Static_task_graph;   // friend: reaches a Guarded's instance + pipe to build node access

namespace detail
{
// Grants the multi-object `ts::access`/`ts::async` builder access to a `Guarded`'s instance + pipe
// (the same internals `Static_task_graph` reaches as a friend). Defined below `Guarded`.
struct Guarded_access;
}

// `Guarded<T>` - the access-controlled wrapper, the sanctioned way to touch a `T` across threads.
// You never receive a bare `T&`; you hand a functor to `access()` (opportunistic - runs inline
// when the object is momentarily free) or `async()` (always enqueued), and it runs once access has
// been granted. The library orders conflicting accesses for you - any number of reads run at once,
// a write runs alone, all in submission order - so a reader never sees a half-finished write and
// two writes never overlap. The mode (read vs write) is deduced from the functor's resource
// parameter (see `access`/`async` below); the free `ts::access`/`ts::async(fn, a, b, ...)` verbs
// extend this to several objects at once in one deadlock-free canonical order, and the same
// ordering backs the static graph's per-node access and the coroutine held-grant guards.
//
// Both verbs take `Access_options` (task.h) = `{token, priority}`; there is deliberately no
// run-inline knob - the verb chooses inline vs enqueued, so the impossible option can't be passed.
template<typename T>
class Guarded
{
    friend class Static_task_graph;
    friend struct detail::Guarded_access;

public:
    // The only constructor: a leading `ts::Named` - a literal or `ts::Named{}` for the
    // construction site - then `T`'s constructor arguments as usual. Naming is required
    // because the name is what every diagnostic, DOT tooltip and trace row about this
    // object prints; `ts::Named{}` is the deliberate "identify me by where I am written"
    // spelling and costs one token.
    //
    // The first parameter must be a `Named`, not something convertible to one:
    // `Named(const char*)` is implicit (so `add_node("physics", ...)` reads well), and
    // without this constraint `Guarded<std::string> g{ "hello" }` would silently mean
    // "named hello, default-constructed string" rather than failing to compile.
    // Not `explicit`: an array of `Guarded` copy-initializes each element from one
    // `ts::Named`, and the type is neither copyable nor movable, so that is the only way
    // to build one in place.
    template<typename N, typename... Args>
        requires std::same_as<std::remove_cvref_t<N>, Named> && std::constructible_from<T, Args...>
    Guarded(N&& name, Args&&... args)
        : instance_(std::forward<Args>(args)...)
    {
        pipe_.debug_name = name;
    }

    // With a declared lock rank (`ts::Rank`, access.h): required only for objects that are
    // dynamically awaited while another grant is held - batch acquisition needs no rank.
    template<typename N, typename... Args>
        requires std::same_as<std::remove_cvref_t<N>, Named> && std::constructible_from<T, Args...>
    Guarded(N&& name, Rank rank, Args&&... args)
        : instance_(std::forward<Args>(args)...)
    {
        pipe_.debug_name = name;
#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
        pipe_.rank = rank.value;
#else
        (void)rank;
#endif
    }

    // Identity matters (it is the access key); waits out pending accesses so the object
    // outlives its last one. The drain is load-bearing even when every access was already
    // `sync()`ed: a task settles in the order settle -> notify waiters -> `on_complete` ->
    // pipe release (`Task_control_block::settle`), so `sync()` returns while the settling
    // thread still holds this object's grant and is about to run
    // `release_and_redispatch` on `pipe_`. `wait_until_idle` is what makes destroying the
    // object right after that `sync()` defined; without it the trailing release lands on
    // freed (or already recycled) pipe state. The waiter cannot outrun the signaler
    // either - the drain notify is done under `Pipe::mutex`, and nothing touches the pipe
    // after that unlock.
    ~Guarded()
    {
#if TS_SAFETY_CHECKS
        if (pipe_.graph_refs.load(std::memory_order_acquire) != 0)
        {
            char label[128];
            char msg[256];
            std::snprintf(msg, sizeof msg,
                "Guarded object '%s' destroyed while a compiled Static_task_graph still "
                "references it (destroy the graph first)",
                named_display(pipe_.debug_name, label, sizeof label));
            ts::fatal(msg);
        }
#endif
        pipe_.wait_until_idle();
    }

    Guarded(const Guarded&) = delete;
    Guarded& operator=(const Guarded&) = delete;

    // Two verbs run a functor under this object's access. Both deduce the mode from the
    // functor's resource parameter - one rule, generic lambdas included:
    //   `T&` / `auto&`               -> read_write (exclusive)
    //   `const T&` / `const auto&`   -> read_only (concurrent readers)
    //   by value or `T&&`            -> rejected (a copy silently discards writes)
    //   `auto&&`                     -> read_only (probed); a mutating body then fails to
    //                                  compile (read bodies receive `const T&`)
    // Non-generic functors are introspected (`accessor_mode`); generic ones are classified by
    // the rvalue-bindability probe. Both accept a trailing `Cancellation_token`, and take
    // `Access_options` = `{token, priority}` (no `run_inline`: the verb is the mode).
    //
    //   access(fn) - opportunistic: runs `fn` on the calling thread when the object is free right
    //                 now (no scheduling), otherwise enqueues. Best for short functors. Because it
    //                 may run inline it can briefly block the caller and stacks its access scope,
    //                 so prefer `async` for anything non-trivial inside a graph node.
    //   async(fn)  - always enqueued off the calling thread. For heavy functors.

    // All four overloads gate on `Read_only_accessor`/`Read_write_accessor` - the
    // mode-first constraint pair (see the concepts for why the order is load-bearing).

    // The trailing `site` on each verb is the naming boundary (ts/named.h): a defaulted
    // `source_location` captures its caller, so it is declared here, on the function the
    // user calls, and the resulting `Named` is passed down explicitly.

    // access, read_write.
    template<typename Fn>
        requires detail::Read_write_accessor<Fn, T>
    auto access(Fn&& fn, Access_options opts = {},
                std::source_location site = std::source_location::current())
        -> Task<detail::Accessor_result_t<Fn, T, Access::read_write>>
    {
        return launch<detail::Accessor_result_t<Fn, T, Access::read_write>, Access::read_write>(
            &instance_, std::forward<Fn>(fn), opts, detail::named_from(opts, site), /*try_inline=*/true);
    }

    // access, read_only.
    template<typename Fn>
        requires detail::Read_only_accessor<Fn, T>
    auto access(Fn&& fn, Access_options opts = {},
                std::source_location site = std::source_location::current()) const
        -> Task<detail::Accessor_result_t<Fn, T, Access::read_only>>
    {
        return launch<detail::Accessor_result_t<Fn, T, Access::read_only>, Access::read_only>(
            &instance_, std::forward<Fn>(fn), opts, detail::named_from(opts, site), /*try_inline=*/true);
    }

    // async, read_write: always enqueued (never inline).
    template<typename Fn>
        requires detail::Read_write_accessor<Fn, T>
    auto async(Fn&& fn, Access_options opts = {},
               std::source_location site = std::source_location::current())
        -> Task<detail::Accessor_result_t<Fn, T, Access::read_write>>
    {
        return launch<detail::Accessor_result_t<Fn, T, Access::read_write>, Access::read_write>(
            &instance_, std::forward<Fn>(fn), opts, detail::named_from(opts, site), /*try_inline=*/false);
    }

    // async, read_only: always enqueued (never inline).
    template<typename Fn>
        requires detail::Read_only_accessor<Fn, T>
    auto async(Fn&& fn, Access_options opts = {},
               std::source_location site = std::source_location::current()) const
        -> Task<detail::Accessor_result_t<Fn, T, Access::read_only>>
    {
        return launch<detail::Accessor_result_t<Fn, T, Access::read_only>, Access::read_only>(
            &instance_, std::forward<Fn>(fn), opts, detail::named_from(opts, site), /*try_inline=*/false);
    }

private:
    template<typename R, Access mode, typename Inst, typename Fn>
    Task<R> launch(Inst* inst, Fn&& fn, Access_options opts, Named name, bool try_inline,
                   detail::Task_ptr* record = nullptr) const
    {
        // The body (stored in the block) runs `fn` under this object's access scope. If
        // `fn` takes a trailing token, the body does too and `Executable::run` forwards the
        // block's token (uniform with the bare-task `ts::launch` path).
        // The context captures the pipe's write-epoch at body start (inside the grant
        // window), so an inherited snapshot that outlives this access goes stale.
        auto core = [&]
        {
            const std::atomic<std::uint64_t>* epoch = detail::pipe_epoch(pipe_);
            const unsigned rank = detail::pipe_rank(pipe_);
            if constexpr (detail::accessor_takes_token_v<Fn, decltype(*inst)>)
            {
                auto body = [inst, epoch, rank, fn = std::forward<Fn>(fn)](const Cancellation_token& tok) mutable -> R
                {
                    Access_context ctx;
                    ctx.add(inst, mode, epoch, rank);
                    Access_scope scope(ctx);
                    return fn(*inst, tok);
                };
                return make_access_block<R>(std::move(body), opts.token);
            }
            else
            {
                auto body = [inst, epoch, rank, fn = std::forward<Fn>(fn)]() mutable -> R
                {
                    Access_context ctx;
                    ctx.add(inst, mode, epoch, rank);
                    Access_scope scope(ctx);
                    return fn(*inst);
                };
                return make_access_block<R>(std::move(body), opts.token);
            }
        }();
        core->flags.priority = opts.priority;
        detail::set_task_name(core, name);
        // Reentrant fast path (`access` only, docs/coroutine-first.md §4.2 / waiting rule (b)):
        // the caller's task already holds this pipe's write grant - run the body inline
        // under that grant, touching the pipe not at all (no acquire, no turn; the held
        // write is exclusive, so a read or write body is equally legal). Queueing instead
        // would park the access behind the caller's own hold. `Executable::run` overwrites
        // the unused pipe-turn lock when it arms the execution counter.
        if (try_inline)
        {
            detail::Task_control_block* owner = pipe_.writer_owner.load(std::memory_order_acquire);
            if (owner != nullptr && owner == detail::current_task.get())
            {
                detail::bind_pipe_link(core.get(), 0, pipe_, mode);   // diagnostics only; never enqueued
                core->execute(core);
                return Task<R>(core);
            }
        }
        detail::bind_pipe_link(core.get(), 0, pipe_, mode);
        core->num_locks.store(1, std::memory_order_relaxed);   // the one pipe turn
        // Inline fast path (`access`): claim an idle pipe and run on this thread; otherwise
        // (or for `async`) enqueue - the admitted turn's release dispatches the body.
        if (try_inline && detail::pipe_try_inline(global_scheduler(), pipe_, mode, core))
            return Task<R>(core);
        detail::pipe_enter_first(core.get(), record);
        return Task<R>(core);
    }

    // The block factory for a single-object access: a `Piped_executable` with one embedded
    // link.
    template<typename R, typename Body>
    static detail::Task_ptr make_access_block(Body&& body, Cancellation_token token)
    {
        return detail::make_piped_executable<R, 1>(std::forward<Body>(body), std::move(token));
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

    // `Deferred::commit`'s enqueue arm: an ordinary write access whose block is recorded
    // into `record` atomically with the enqueue (under the pipe mutex - see
    // `pipe_enqueue`), so the recorded handle can never lag FIFO order.
    template<typename T, typename Fn>
    static Task<void> commit_write(Guarded<T>& t, Fn&& fn, Access_options opts, Named name, Task_ptr* record)
    {
        return t.template launch<void, Access::read_write>(
            &t.instance_, std::forward<Fn>(fn), opts, name, /*try_inline=*/false, record);
    }
};

// Snapshot a slot written under `pipe`'s admission serialization (`Deferred`'s recorded
// last commit) with the same ordering: on the mutex pipe, under its mutex. A destructor
// racing live commits is already a use-after-free by contract; the lock only keeps the
// sanctioned quiescent read well-defined.
inline Task_ptr pipe_locked_snapshot(Pipe& pipe, const Task_ptr& slot)
{
    std::scoped_lock lock(pipe.mutex);
    return slot;
}

// An access-mode-tagged object argument, produced by `ts::as_read_only(g)` / `ts::as_read_write(g)`. It
// lets a generic lambda (`[](auto& x){...}`) declare per-object access explicitly: a generic
// lambda's `operator()` is a template with no introspectable parameter const-ness, so
// `Function_traits` cannot deduce read-vs-write - the tag supplies it. The mode is a compile-time
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
// not be mixed - see the `static_assert` at each entry point.
template<typename A>
concept Object_arg = is_guarded_v<A> || is_access_arg_v<A>;

// The one multi-object builder: `fn(...)` under an `Access_context` declaring every object at
// its (compile-time) `Modes...`, gated on holding all their pipes. Read positions are invoked
// with `const T&` (`mode_ref`) so a mutating body under a read classification fails to compile.
// The deduced / probed / tagged entry paths differ only in where `Modes...` come from.
template<Access... Modes, std::size_t... I, typename Fn, typename... Ts>
auto async_build_modes(Access_options opts, std::index_sequence<I...>, Fn&& fn, Guarded<Ts>&... objs)
{
    using R = std::invoke_result_t<Fn, Mode_ref_t<Modes, Ts>...>;
    auto instances = std::make_tuple(Guarded_access::instance(objs)...);
    auto epochs = std::make_tuple(pipe_epoch(Guarded_access::pipe(objs))...);
    auto ranks = std::make_tuple(pipe_rank(Guarded_access::pipe(objs))...);

    auto body = [instances, epochs, ranks, fn = std::forward<Fn>(fn)]() mutable -> R
    {
        Access_context ctx;
        (ctx.add(static_cast<const void*>(std::get<I>(instances)), Modes,
                 std::get<I>(epochs), std::get<I>(ranks)), ...);
        Access_scope scope(ctx);
        return fn(mode_ref<Modes>(std::get<I>(instances))...);
    };
    Pipe* pipes[] = { &Guarded_access::pipe(objs)... };
    Access modes[] = { Modes... };
    auto block = make_piped_executable<R, sizeof...(Ts)>(std::move(body), std::move(opts.token));
    block->flags.priority = opts.priority;
    // Multi-object `ts::access`/`ts::async` end in an object pack, so no trailing defaulted
    // `source_location` is expressible and there is no call site to capture: an unnamed
    // multi-object access carries only whatever literal the options gave it. Name it with
    // `{.name = "..."}` when a diagnostic would need to point at it.
    Named name{ nullptr };
    name.literal = opts.name;
    set_task_name(block, name);
    // Insertion-sort by pipe address (canonical order), in place - the pack is small. A
    // repeated object is fatal: declare each object once, with the strongest mode the body
    // needs (a duplicate is a copy-paste bug far more often than intent; this was previously
    // a silent write-wins dedup and can be relaxed back on demand, e.g. for generically
    // assembled object packs).
    std::size_t n = 0;
    for (std::size_t k = 0; k < sizeof...(Ts); ++k)
    {
        Pipe* pk = pipes[k];
        Access mk = modes[k];
        std::size_t i = 0;
        while (i < n && pipes[i] < pk)
            ++i;
        if (i < n && pipes[i] == pk)
            ts::fatal("ts::access/ts::async: the same Guarded object was passed twice - declare "
                      "each object once, with the strongest access the body needs");
        for (std::size_t j = n; j > i; --j)
        {
            pipes[j] = pipes[j - 1];
            modes[j] = modes[j - 1];
        }
        pipes[i] = pk;
        modes[i] = mk;
        ++n;
    }
    for (std::size_t i = 0; i < n; ++i)
        bind_pipe_link(block.get(), static_cast<std::uint8_t>(i), *pipes[i], modes[i]);
    block->num_locks.store(static_cast<std::uint32_t>(n), std::memory_order_relaxed);
    Task<R> result(block);
    pipe_enter_first(block.get());   // turns cascade canonically; the last release dispatches
    return result;
}

// Deduced path (bare args, introspectable functor): modes from the functor's parameter
// const-ness; by-value / rvalue-ref resource parameters rejected.
template<typename Args, std::size_t... I, typename Fn, typename... Ts>
auto async_build(Access_options opts, std::index_sequence<I...> seq, Fn&& fn, Guarded<Ts>&... objs)
{
    static_assert((std::is_lvalue_reference_v<std::tuple_element_t<I, Args>> && ...),
        "a guarded-resource parameter must be `T&` or `const T&`: taking it by value copies "
        "the resource (writes hit the copy and are silently discarded), and `T&&` cannot "
        "bind the stored instance");
    return async_build_modes<async_mode_of<std::tuple_element_t<I, Args>>()...>(
        std::move(opts), seq, std::forward<Fn>(fn), objs...);
}

// Probed path (bare args, generic functor): modes from the per-position rvalue probe -
// `const auto&`/`auto&&` = read, `auto&` = write. No tags needed.
template<std::size_t... I, typename Fn, typename... Ts>
auto async_build_probed(Access_options opts, std::index_sequence<I...> seq, Fn&& fn, Guarded<Ts>&... objs)
{
    static_assert(std::invocable<Fn, Ts&...>,
        "multi-object access/async: functor parameters must match the Guarded objects "
        "(same arity, each taken by reference)");
    // Guard the forward on the same condition: a failed assert does not stop
    // instantiation, so without it `async_build_modes` re-errors past the message.
    if constexpr (std::invocable<Fn, Ts&...>)
        return async_build_modes<probed_mode<Fn, I, Ts...>()...>(
            std::move(opts), seq, std::forward<Fn>(fn), objs...);
}

// Tagged path (`ts::as_read_only`/`as_read_write` on every arg): modes from the tags.
template<std::size_t... I, typename Fn, typename... Objs>
auto async_build_tagged(Access_options opts, std::index_sequence<I...> seq, Fn&& fn, Objs&&... objs)
{
    return async_build_modes<std::remove_cvref_t<Objs>::mode...>(
        std::move(opts), seq, std::forward<Fn>(fn), *objs.obj...);
}

} // namespace detail

// Tag an object argument with an explicit access mode: `graph.add_node([](auto& p, auto& n)
// { n.q(p); }, ts::as_read_write(physics), ts::as_read_only(nav))`, and likewise
// `ts::access`/`ts::async`. Tags are never required - modes are deduced from parameter
// const-ness for non-generic functors and probed (`const auto&` = read, `auto&` = write) for
// generic ones - but remain for those preferring explicit declaration at the call site, and
// as the escape hatch for an `auto&&` parameter that must write. The tag wins over the
// parameter spelling; an over-declared write (write tag, `const T&` parameter) is legal
// conservative serialization. (Named `as_*` to avoid colliding with the coroutine access guards
// `ts::read_only`/`ts::read_write` in coroutine_support.h.)
template<typename T>
detail::Access_arg<T, Access::read_only> as_read_only(Guarded<T>& g) { return { &g }; }
template<typename T>
detail::Access_arg<T, Access::read_write> as_read_write(Guarded<T>& g) { return { &g }; }

// Multi-object async: run `fn(obj1, obj2, ...)` once it holds all the objects. Per-object
// access, one rule (same as single-object): parameter const-ness for a non-generic functor
// (`const T&` = read, `T&` = write; by-value / `T&&` rejected), the rvalue probe for a generic
// one (`const auto&`/`auto&&` = read, `auto&` = write), or explicit `ts::as_read_only` /
// `as_read_write` tags on every argument (don't mix tagged and bare in one call). Read
// positions receive `const T&`, so a mutating body under a read classification does not
// compile. Deadlock-free (objects acquired in canonical order). Options come first (a function
// parameter pack can't be followed by a defaulted arg); the no-options overload defaults them.
// `token`/`priority` apply as usual. Fire-and-forget or consume the `Task<R>` - but do not
// block a graph node on it (same rule as single-object async).
template<typename Fn, typename... Objs>
    requires (sizeof...(Objs) >= 1) && (detail::Object_arg<Objs> && ...)
auto async(Access_options opts, Fn&& fn, Objs&&... objs)
{
    constexpr bool any_tagged = (detail::is_access_arg_v<Objs> || ...);
    if constexpr (any_tagged)
    {
        static_assert((detail::is_access_arg_v<Objs> && ...),
            "multi-object async: don't mix tagged (ts::as_read_only/as_read_write) and bare Guarded "
            "arguments - tag EVERY object argument, or tag none");
        return detail::async_build_tagged(std::move(opts), std::index_sequence_for<Objs...>{},
            std::forward<Fn>(fn), std::forward<Objs>(objs)...);
    }
    else if constexpr (detail::introspectable_v<Fn>)
    {
        using Args = typename detail::Function_traits<std::decay_t<Fn>>::args;
        static_assert(std::tuple_size_v<Args> == sizeof...(Objs),
            "multi-object async: functor arity must match the number of Guarded objects");
        return detail::async_build<Args>(std::move(opts), std::index_sequence_for<Objs...>{},
            std::forward<Fn>(fn), objs...);
    }
    else
    {
        return detail::async_build_probed(std::move(opts), std::index_sequence_for<Objs...>{},
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
// like `async` (always enqueued) - unlike single-object `access`, which runs inline when the
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

// `ts::launch` (bare scheduler task) lives in task.h now - it dispatches
// through the `submit_ready` bridge and touch no `Guarded`, so they belong with the task core.

} // namespace ts
