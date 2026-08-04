#pragma once

#include "ts/access.h"
#include "ts/detail/pipe_link.h"
#include "ts/scheduler.h"
#include "ts/task.h"

#include <concepts>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ts
{

// Ambient scheduler used by `access()` / `async()` (v1: a process-wide default).
Scheduler& global_scheduler();

namespace detail
{

// Submit a closure to the scheduler (bridges to the raw func-ptr API).
void submit_closure(Scheduler& scheduler, std::move_only_function<void()> closure,
                    Priority priority = Priority::normal);

// A per-object reader/writer pipe (the evolved mutex pipe, docs/pipe-rebase.md §0.2).
// Entries are admitted in FIFO order; consecutive readers run concurrently, a writer runs
// alone (no readers, no other writer). Different objects have independent pipes and run in
// parallel. Non-blocking: callers never wait; admission is completion-driven, and an
// admitted entry's turn fires `release()` on its owner -- the pipe is a prerequisite
// source for the block machinery, not a dispatcher. The queue is the tasks' own embedded
// `Pipe_link`s threaded intrusively, so queueing allocates nothing.
struct Pipe
{
    std::mutex mutex;
    std::condition_variable idle;
    Pipe_link* queue_head = nullptr;   // waiting (not yet admitted) entries, FIFO
    Pipe_link* queue_tail = nullptr;
    int active_readers = 0;
    bool writer_active = false;
#if TS_SAFETY_CHECKS
    // Grant-window epoch for the harness's stale-inherited-grant check (see
    // `Access_context`). Seqlock-style parity: bumped at every write-grant acquire and
    // release (always under `mutex`), so it is odd while a writer holds and even during
    // reader eras. Reader
    // acquires/releases do not bump -- a read grant goes stale exactly when a writer
    // acquires after its capture, which is the actual safety condition. Fully compiled
    // out with the harness (the gating convention: safety-only state carries no
    // shipping cost; mixed-config TUs are an ODR violation caught by the link-time
    // tripwire in access.h).
    std::atomic<std::uint64_t> write_epoch{ 0 };
    // Count of compiled `Static_task_graph`s whose `distinct_pipes_` reference this pipe
    // (`compile()` +1; graph destruction, recompile, and move-assign-overwrite -1).
    // `~Guarded` fatals while it is nonzero: destroying the object would leave the graph
    // holding dangling pipe/instance pointers for its next run (the Taskflow-#82 class of
    // lifetime misuse, caught at the cause instead of crashing far from it).
    std::atomic<int> graph_refs{ 0 };
#endif
    // Identity of the owning `Guarded`/`Versioned` (`ts::Named`): a literal or the
    // construction site, referenced not copied. Consumed by the graph's DOT dump (edge
    // tooltips), the trace, and the access diagnostics; kept in all builds (three words --
    // a handful of objects, and the consumers are shipping-capable).
    Named debug_name{ nullptr };
#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
    // The object's declared `ts::Rank`; 0 = unranked (the strict default -- see `ts::Rank`).
    unsigned rank = 0;
#endif

    // The block currently holding this pipe's WRITE grant, null outside a write window
    // (docs/pipe-rebase.md §0.2). Always-on: behavior keys off it (`Deferred::commit`
    // applies inline when the caller IS the holder), so it cannot live behind
    // `TS_SAFETY_CHECKS`. Written under `mutex` at write admission/release (plus the
    // graph's direct write handoff, which runs inside an exclusive write window); read
    // lock-free by the ownership check. Identity only -- never dereferenced.
    std::atomic<Task_control_block*> writer_owner{ nullptr };

    // Blocks until the pipe is fully drained and nothing is in flight. Teardown-only
    // (`~Guarded`). Safe against the completing job that signals it: the notify is done
    // under `mutex` (`release_and_redispatch`), so this waiter cannot rewake and destroy
    // the pipe until it re-acquires `mutex` after the signaler's notify returns -- no
    // UE-`FPipe`-class refcounted drain event needed (that race is a lock-free-notify
    // artifact). See `release_and_redispatch`.
    void wait_until_idle()
    {
        std::unique_lock lock(mutex);
        idle.wait(lock, [this]
        {
            return queue_head == nullptr && active_readers == 0 && !writer_active;
        });
    }
};

// Grant-epoch source for `Access_context::add`: the pipe's `write_epoch` under
// `TS_SAFETY_CHECKS`, null with the harness compiled out (the three-argument `add`
// ignores a null source). Lets the capture sites -- which compile in both configs --
// name the fully-gated field through one spelling.
inline const std::atomic<std::uint64_t>* pipe_epoch([[maybe_unused]] const Pipe& pipe) noexcept
{
#if TS_SAFETY_CHECKS
    return &pipe.write_epoch;
#else
    return nullptr;
#endif
}

// The pipe's declared `ts::Rank`, or 0 when the rank rule is compiled out. Same shape as
// `pipe_epoch`: one spelling for the capture sites, which compile in every config.
inline unsigned pipe_rank([[maybe_unused]] const Pipe& pipe) noexcept
{
#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
    return pipe.rank;
#else
    return 0;
#endif
}

// Enter a pipe task's first link into its pipe's queue (starting the sequential canonical
// cascade for a multi-object task -- each admitted turn enters the owner's next link and
// fires `release(owner)`; the last turn's release dispatches through the standard
// `dispatch_ready`). The block must have its links bound (`bind_pipe_link`) and
// `num_locks` seeded with `pipe_count`. `record`, when non-null, receives the block WHILE
// STILL UNDER the first pipe's mutex -- the atomic enqueue-and-record `Deferred::commit`
// needs so its recorded handle can never lag the pipe's FIFO order (the race the deleted
// `commit_mutex_` used to close).
void pipe_enter_first(Task_control_block* blk, Task_ptr* record);   // default arg on the task.h declaration

// Advance (release) every entered link of a settled pipe task. The settle-must-advance-
// links contract: every pipe-task creation site must route its settle through this --
// `make_piped_executable` installs it as `on_complete`; the graph's `graph_node_completed`
// calls it first. A missed call wedges the affected pipes.
void advance_pipe_links(Task_control_block* blk);

// `on_complete` hook form of the above, for tasks whose settle needs nothing else.
void pipe_links_on_complete(Task_control_block* blk);

// Acquire a pipe as a held grant in `mode` (the coroutine guards' primitive -- everything
// else rides links on its own block): a `read_only` hold joins concurrent readers, a
// `read_write` hold is exclusive, released only by `pipe_release`. Returns true if
// acquired in-call (no callback fires); false if deferred, in which case `on_acquired`
// runs once the pipe drains to it (FIFO), scheduled on `scheduler`. `owner` is the
// grant-holder block a WRITE hold publishes through `Pipe::writer_owner` (null when no
// block identity exists). A deferred hold allocates one small queue node -- the one
// allocating pipe path.
bool pipe_acquire(Scheduler& scheduler, Pipe& pipe, Access mode, std::move_only_function<void()> on_acquired,
                  Task_control_block* owner = nullptr);

// Release a hold taken by `pipe_acquire` in `mode`; admits queued entries.
void pipe_release(Scheduler& scheduler, Pipe& pipe, Access mode);

#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)
// Waits-for cycle detector (docs/coroutine-first.md §2). At a genuine suspension on a
// pipe -- a deferred coroutine guard acquire, or awaiting a pipe-job task -- the awaiters
// record one edge per held grant: {pipe the suspending context holds -> pipe it awaits}.
// Held pipes come from `held`'s epoch sources; a cycle among the recorded edges is the
// suspended-ABBA deadlock (no thread parks -- both frames are suspended and every worker
// is free -- so nothing would ever diagnose it at runtime), fatal at the moment the
// closing edge is inserted, naming both tasks and both objects. `ticket` identifies this
// suspension (the awaiter's address); `waiter` is the suspending task's block for the
// diagnostic (may be null off-task). Returns whether any edge was recorded, so the resume
// path knows to `circular_wait_clear`. Insertion and the cycle check share one registry lock,
// so a true deadlock is always caught by whichever awaiter inserts last. A granted-but-
// not-yet-cleared edge (the grant fires between record and resume) can in principle close
// a spurious cycle in that window; the window is a few instructions on the resume path and
// requires a reader-share interleaving to matter -- accepted for a safety harness.
bool circular_wait_record(const Access_context* held, const void* ticket, const Task_control_block* waiter,
                      Pipe* const* awaited, int count);
void circular_wait_clear(const void* ticket) noexcept;
#endif

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
template<typename C, typename R, typename... A>
struct Function_traits<R(C::*)(A...) noexcept> { using args = std::tuple<A...>; };
template<typename C, typename R, typename... A>
struct Function_traits<R(C::*)(A...) const noexcept> { using args = std::tuple<A...>; };
template<typename R, typename... A>
struct Function_traits<R(*)(A...)> { using args = std::tuple<A...>; };
template<typename R, typename... A>
struct Function_traits<R(*)(A...) noexcept> { using args = std::tuple<A...>; };

// `read_only` for a `const T&` parameter, `read_write` otherwise.
template<typename Arg>
constexpr Access async_mode_of()
{
    return std::is_const_v<std::remove_reference_t<Arg>> ? Access::read_only : Access::read_write;
}

// SFINAE-friendly introspectability: a class with a single, non-template `operator()` (or a
// function pointer/reference). A generic lambda's `operator()` is a TEMPLATE -- taking its
// address without arguments is ill-formed -- so it lands in the `false` case and access modes
// are classified by the rvalue probe below instead of `Function_traits`.
template<typename Fn, typename = void>
struct has_introspectable_call : std::false_type {};
template<typename Fn>
struct has_introspectable_call<Fn, std::void_t<decltype(&Fn::operator())>> : std::true_type {};

template<typename Fn>
inline constexpr bool introspectable_v =
    has_introspectable_call<std::remove_cvref_t<Fn>>::value
    || std::is_function_v<std::remove_pointer_t<std::remove_cvref_t<Fn>>>;

// Rvalue-bindability probe for GENERIC functors: position `P` gets an rvalue `T&&`, every
// other position an lvalue `T&`. `auto&` cannot bind an rvalue ([temp.deduct.call]) ->
// `read_write`; `const auto&` / `auto&&` can -> `read_only`. Declaration-level only -- the
// body is never instantiated by the probe, so this is exact and SFINAE-safe. (An `auto&&`
// parameter that mutates is mis-probed as a read, but Part of the same design makes that a
// compile error: read positions are invoked with `const T&`, so `auto&&` deduces `const T&`
// and the mutation fails to compile. The one undetectable residual is an `auto` BY-VALUE
// parameter -- it probes as a read and copies; writes hit the copy. Documented in the guide.)
template<std::size_t P, std::size_t K, typename T>
using Probe_arg_t = std::conditional_t<P == K, T&&, T&>;

template<typename Fn, std::size_t P, typename... Ts, std::size_t... K>
constexpr bool probe_binds_rvalue(std::index_sequence<K...>)
{
    return std::invocable<Fn, Probe_arg_t<P, K, Ts>...>;
}

template<typename Fn, std::size_t P, typename... Ts>
constexpr Access probed_mode()
{
    return probe_binds_rvalue<Fn, P, Ts...>(std::index_sequence_for<Ts...>{})
        ? Access::read_only : Access::read_write;
}

// Per-position access-corrected reference: a read position hands the body `const T&`, so a
// mutating body under a read classification fails to COMPILE (structural const-correctness,
// on top of the runtime harness); a write position hands `T&`.
template<Access M, typename T>
using Mode_ref_t = std::conditional_t<M == Access::read_only, const T&, T&>;

template<Access M, typename T>
constexpr Mode_ref_t<M, T> mode_ref(T* p) { return *p; }

// Deduced access mode of a single-object accessor `Fn` against payload `T`:
//  - introspectable (non-generic lambda / functor / function pointer): the resource
//    parameter's const-ness decides -- `const T&` = read_only, `T&` = read_write. A by-value
//    or rvalue-ref resource parameter is rejected outright.
//  - generic (templated `operator()`): the rvalue probe -- `const auto&`/`auto&&` = read_only,
//    `auto&` = read_write. Token-arity aware (a trailing `Cancellation_token` is allowed).
template<typename Fn, typename T>
constexpr Access accessor_mode()
{
    if constexpr (introspectable_v<Fn>)
    {
        using Args = typename Function_traits<std::decay_t<Fn>>::args;
        static_assert(std::tuple_size_v<Args> >= 1,
            "a guarded accessor must take the resource as its first parameter");
        // Short-circuit on the failed arity assert: forming `Arg0` for an empty
        // parameter list would add a second, misleading tuple-out-of-bounds cascade
        // after the message above. The dummy return only feeds a constraint that
        // then fails cleanly at the caller.
        if constexpr (std::tuple_size_v<Args> >= 1)
        {
            using Arg0 = std::tuple_element_t<0, Args>;
            static_assert(std::is_lvalue_reference_v<Arg0>,
                "a guarded-resource parameter must be `T&` or `const T&`: taking it by value copies "
                "the resource (writes hit the copy and are silently discarded), and `T&&` cannot "
                "bind the stored instance");
            return async_mode_of<Arg0>();
        }
        else
        {
            return Access::read_only;
        }
    }
    else
    {
        return (std::invocable<Fn, T&&> || std::invocable<Fn, T&&, const Cancellation_token&>)
            ? Access::read_only : Access::read_write;
    }
}

// An executable pipe task: the `Executable` wrapper plus its embedded per-pipe links --
// ONE allocation for block + result + body + links (docs/pipe-rebase.md §0.2). `exec` is
// the first member and `Executable::core` its first, so the intrusive handle aliases the
// whole wrapper as usual.
template<typename Body, typename R, std::size_t N>
struct Piped_executable
{
    Executable<Body, R> exec;
    Pipe_link links[N];

    explicit Piped_executable(Body b)
        : exec(std::move(b))
    {}
};

// Build a pipe task with capacity for `N` links (bind them with `bind_pipe_link`, then set
// `num_locks` to the bound count and enter). Installs `pipe_links_on_complete` -- the
// settle-must-advance-links contract's factory half (the graph's node blocks are the other
// creation site; see `graph_node_completed`).
template<typename R, std::size_t N, typename Body>
Task_ptr make_piped_executable(Body&& body, Cancellation_token token)
{
    using Exec = Executable<std::decay_t<Body>, R>;
    using Wrapper = Piped_executable<std::decay_t<Body>, R, N>;
    auto* w = new Wrapper(std::forward<Body>(body));
    Task_control_block& core = w->exec.core;
    core.destroy = [](Task_control_block* c) { delete reinterpret_cast<Wrapper*>(c); };
    core.execute = &Exec::run;
    core.on_complete = &pipe_links_on_complete;
    core.token = std::move(token);
    core.pipe_links = w->links;
    return Task_ptr(&core);   // refcount 0 -> 1, owns the wrapper
}

// Bind the block's next link to (pipe, mode). Canonical order is the CALLER's contract:
// links must be bound in ascending pipe-address order (single-object trivially; the
// multi-object builder sorts; the graph's `pipe_indices` are ascending over the
// address-sorted `distinct_pipes_`).
inline void bind_pipe_link(Task_control_block* core, std::uint8_t index, Pipe& pipe, Access mode)
{
    Pipe_link& l = core->pipe_links[index];
    l.owner = core;
    l.pipe = &pipe;
    l.index = index;
    l.mode = mode;
    core->pipe_count = static_cast<std::uint8_t>(index + 1);
}

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

// The accessor gates: mode classification FIRST, invocability second -- the order is
// load-bearing. `accessor_mode` classifies without ever instantiating a body
// (introspection / the rvalue probe), while `Async_accessor` probes invocability --
// and probing a GENERIC lambda deduces its return type, which instantiates the BODY;
// for a mutating body probed against `const T&` that is a hard error, not a
// substitution failure. Conjunctions short-circuit, so a failed mode gate rejects
// cleanly at the caller and the probe is never evaluated for the wrong mode. The one
// spelling for every read/write accessor position (`Guarded`'s verbs,
// `Versioned::read`).
template<typename Fn, typename T>
concept Read_only_accessor = (accessor_mode<Fn, T>() == Access::read_only)
    && Async_accessor<Fn, const T&>;

template<typename Fn, typename T>
concept Read_write_accessor = (accessor_mode<Fn, T>() == Access::read_write)
    && Async_accessor<Fn, T&>;

template<typename Fn, typename A>
inline constexpr bool accessor_takes_token_v = std::invocable<Fn, A, const Cancellation_token&>;

// Result type of an accessor, picking the token-taking overload when present.
template<typename Fn, typename A, bool = accessor_takes_token_v<Fn, A>>
struct Async_result_sel { using type = std::invoke_result_t<Fn, A, const Cancellation_token&>; };
template<typename Fn, typename A>
struct Async_result_sel<Fn, A, false> { using type = std::invoke_result_t<Fn, A>; };

// Result type for the `M`-mode accessor overload, guarded twice. It is `void` (benign) unless
// the functor is invocable AND actually CLASSIFIES as `M` -- so a rejected overload's trailing
// return type never computes `invoke_result_t`. Two hard errors hide behind an unguarded
// compute: MSVC evaluates a rejected overload's return type during overload resolution where
// clang SFINAEs it away (C2794/C2938 on a non-invocable combination), and for a GENERIC lambda
// a deduced return type requires instantiating the BODY -- instantiating a mutating body
// against `const T&` would hard-error inside the overload that was never going to be chosen.
// LAYERED on purpose (not one `&&` expression): MSVC eagerly satisfies a concept-id in a
// default template argument even when the left operand of `&&` is already false, which would
// re-trigger the body instantiation the mode gate exists to prevent. Specialization makes the
// invocability probe structurally unreachable on a mode mismatch.
template<typename Fn, typename A,
         bool = std::invocable<Fn, A> || accessor_takes_token_v<Fn, A>>
struct Accessor_result_checked { using type = void; };
template<typename Fn, typename A>
struct Accessor_result_checked<Fn, A, true> : Async_result_sel<Fn, A> {};

template<typename Fn, typename T, Access M,
         bool = (accessor_mode<Fn, T>() == M)>   // gate FIRST: classifies without body instantiation
struct Accessor_result { using type = void; };
template<typename Fn, typename T, Access M>
struct Accessor_result<Fn, T, M, true> : Accessor_result_checked<Fn, Mode_ref_t<M, T>> {};

template<typename Fn, typename T, Access M>
using Accessor_result_t = typename Accessor_result<Fn, T, M>::type;

} // namespace detail

// `Guarded::access`/`async` and the multi-object `ts::access`/`ts::async` take `Access_options`
// (defined in task.h) = `{token, priority}`. There is deliberately no run-inline knob -- the
// verb chooses inline vs enqueued (`access` inline-when-free, `async` always enqueued), so the
// impossible option can't be passed.

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
    // The ONLY constructor: a leading `ts::Named` -- a literal or `ts::Named{}` for the
    // construction site -- then `T`'s constructor arguments as usual. Naming is required
    // because the name is what every diagnostic, DOT tooltip and trace row about this
    // object prints; `ts::Named{}` is the deliberate "identify me by where I am written"
    // spelling and costs one token.
    //
    // The first parameter must BE a `Named`, not something convertible to one:
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
    // DYNAMICALLY AWAITED while another grant is held -- batch acquisition needs no rank.
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
    // either -- the drain notify is done under `Pipe::mutex`, and nothing touches the pipe
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
                "references it (destroy or recompile the graph first)",
                named_display(pipe_.debug_name, label, sizeof label));
            ts::fatal(msg);
        }
#endif
        pipe_.wait_until_idle();
    }

    Guarded(const Guarded&) = delete;
    Guarded& operator=(const Guarded&) = delete;

    // Two verbs run a functor under this object's access. Both deduce the mode from the
    // functor's resource parameter -- ONE rule, generic lambdas included:
    //   `T&` / `auto&`               -> read_write (exclusive)
    //   `const T&` / `const auto&`   -> read_only (concurrent readers)
    //   by value or `T&&`            -> rejected (a copy silently discards writes)
    //   `auto&&`                     -> read_only (probed); a mutating body then fails to
    //                                  compile (read bodies receive `const T&`)
    // Non-generic functors are introspected (`accessor_mode`); generic ones are classified by
    // the rvalue-bindability probe. Both accept a trailing `Cancellation_token`, and take
    // `Access_options` = `{token, priority}` (no `run_inline`: the verb IS the mode).
    //
    //   access(fn) -- opportunistic: runs `fn` on the CALLING thread when the object is free right
    //                 now (no scheduling), otherwise enqueues. Best for short functors. Because it
    //                 may run inline it can briefly block the caller and stacks its access scope,
    //                 so prefer `async` for anything non-trivial inside a graph node.
    //   async(fn)  -- always enqueued off the calling thread. For heavy functors.

    // All four overloads gate on `Read_only_accessor`/`Read_write_accessor` -- the
    // mode-first constraint pair (see the concepts for why the order is load-bearing).

    // The trailing `site` on each verb is the naming boundary (ts/named.h): a defaulted
    // `source_location` captures its CALLER, so it is declared here, on the function the
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
        // block's token (uniform with the bare-task path's `with_inherited_access`).
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
        // the caller's task already holds this pipe's write grant -- run the body inline
        // UNDER that grant, touching the pipe not at all (no acquire, no turn; the held
        // write is exclusive, so a read or write body is equally legal). Queueing instead
        // would park the access behind the caller's own hold. `Executable::run` overwrites
        // the unused pipe-turn lock when it arms the execution counter.
        if (try_inline)
        {
            detail::Task_control_block* owner = pipe_.writer_owner.load(std::memory_order_acquire);
            if (owner != nullptr && owner == detail::current_task.get())
            {
                detail::bind_pipe_link(core.get(), 0, pipe_, mode);   // diagnostics only; never enqueued
                core->execute(core, 0);
                return Task<R>(core);
            }
        }
        detail::bind_pipe_link(core.get(), 0, pipe_, mode);
        core->num_locks.store(1, std::memory_order_relaxed);   // the one pipe turn
        // Inline fast path (`access`): claim an idle pipe and run on this thread; otherwise
        // (or for `async`) enqueue -- the admitted turn's release dispatches the body.
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
    // into `record` atomically with the enqueue (under the pipe mutex -- see
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
    // Dedup write-wins + insertion-sort by pipe address (canonical order), in place -- the
    // pack is small, and this replaces the old per-call `std::map`.
    std::size_t n = 0;
    for (std::size_t k = 0; k < sizeof...(Ts); ++k)
    {
        Pipe* pk = pipes[k];
        Access mk = modes[k];
        std::size_t i = 0;
        while (i < n && pipes[i] < pk)
            ++i;
        if (i < n && pipes[i] == pk)
        {
            if (mk == Access::read_write)
                modes[i] = Access::read_write;   // write wins on a repeated object
            continue;
        }
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

// Probed path (bare args, GENERIC functor): modes from the per-position rvalue probe --
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

// Tag an object argument with an EXPLICIT access mode: `graph.add_node([](auto& p, auto& n)
// { n.q(p); }, ts::as_read_write(physics), ts::as_read_only(nav))`, and likewise
// `ts::access`/`ts::async`. Tags are never required -- modes are deduced from parameter
// const-ness for non-generic functors and probed (`const auto&` = read, `auto&` = write) for
// generic ones -- but remain for those preferring explicit declaration at the call site, and
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
// compile. Deadlock-free (objects acquired in canonical order). Options come FIRST (a function
// parameter pack can't be followed by a defaulted arg); the no-options overload defaults them.
// `token`/`priority` apply as usual. Fire-and-forget or consume the `Task<R>` -- but do NOT
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
            "arguments -- tag EVERY object argument, or tag none");
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
// through the `submit_ready` bridge and touch no `Guarded`, so they belong with the task core.

} // namespace ts
