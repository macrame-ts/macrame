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

#include <atomic>
#include <concepts>
#include <cstdio>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <source_location>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ts
{

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

template<typename T> class Guarded;
template<typename... Args> class Access_op;

// Tag for the bound-but-dormant `Access_op` constructor: `Access_op(ts::dormant, world, body)`
// stores the target and body without firing - `start()` fires later. Needed because a member
// init list cannot call `bind()`. (Named `dormant`, not `defer` - `Deferred` is taken.)
struct Dormant {};
inline constexpr Dormant dormant{};

namespace detail
{
// An access-mode-tagged object argument (`ts::as_read_only`/`as_read_write`), defined below
// `Guarded`; declared here because an `Access_op` position reads the tag for its mode.
template<typename T, Access M> struct Access_arg;

// `Access_op::sync() &`'s return: a non-consuming `const R&` peek, `void` for a void access.
template<typename R2> struct Sync_ref { using type = const R2&; };
template<> struct Sync_ref<void> { using type = void; };
template<typename R2> using Sync_ref_t = typename Sync_ref<R2>::type;

// The op's hooks for the coroutine awaiter (coroutine_support.h). Defined below the class.
template<typename... Args>
Task_control_block* access_op_core(Access_op<Args...>& op) noexcept;
template<typename... Args>
bool* access_op_consumed(Access_op<Args...>& op) noexcept;
template<typename... Args>
bool access_op_started(const Access_op<Args...>& op) noexcept;

// Constructs an `Access_op` in the caller's storage from the verb's flattened targets. The
// verbs reach `Guarded`'s internals through `Guarded_access`; this is the other half, giving
// them the op's private constructor without friending every entry path.
struct Access_op_maker;

// What `Access_op<...>::as_optional()` returns: the op's core plus its this-cycle consume
// flag, made awaitable by an `operator co_await` in coroutine_support.h. The op-shaped
// counterpart of `Optional_awaitable` (task.h), which carries no consume flag because a
// `Task`'s result lives in a refcounted block rather than caller storage.
template<typename R2> struct Optional_access_awaitable
{
    Task_ptr core;
    bool* consumed;
};

// One position of an `Access_op`'s object list: a bare `T`, whose mode the body decides, or an
// `Access_arg<T, M>` from a call-site tag, which declares it.
template<typename A>
struct Op_object
{
    using type = A;
    static constexpr bool tagged = false;
    static constexpr Access mode = Access::read_only;   // unread: the deduction below decides
};

template<typename T, Access M>
struct Op_object<Access_arg<T, M>>
{
    using type = T;
    static constexpr bool tagged = true;
    static constexpr Access mode = M;
};

// The access mode of object position `P` - the one spelling rule (docs/guide.md §5), in the
// tiers the verbs already use: a call-site tag wins; a single-object op is classified by
// `accessor_mode` (which is also token-arity aware, so `Access_op<T, Body>` keeps exactly the
// mode it had before the type became variadic); a multi-object introspectable body by its
// parameter's const-ness; a generic one by the per-position rvalue probe.
template<typename Body, std::size_t P, typename Arg, typename... Objs>
constexpr Access op_mode_of()
{
    if constexpr (Op_object<Arg>::tagged)
        return Op_object<Arg>::mode;
    else if constexpr (sizeof...(Objs) == 1)
        return accessor_mode<Body, Objs...>();
    else if constexpr (introspectable_v<Body>)
        return async_mode_of<std::tuple_element_t<P, typename Function_traits<std::decay_t<Body>>::args>>();
    else
        return probed_mode<Body, P, Objs...>();
}

template<typename Body, std::size_t P, typename Arg, typename Objects> struct Op_mode;
template<typename Body, std::size_t P, typename Arg, typename... Objs>
struct Op_mode<Body, P, Arg, std::tuple<Objs...>>
{
    static constexpr Access value = op_mode_of<Body, P, Arg, Objs...>();
};

// The op's result over its mode-corrected object references, with the opt-in trailing
// `Cancellation_token`. Guarded like `Accessor_result`: a combination the body cannot be
// invoked with yields `void` instead of hard-erroring inside `invoke_result_t`.
template<bool Invocable, bool Takes_token, typename Body, typename... As>
struct Op_result_sel { using type = void; };
template<bool Takes_token, typename Body, typename... As>
struct Op_result_sel<true, Takes_token, Body, As...> : Node_body_result<Takes_token, Body, As...> {};

template<typename Body, typename Refs> struct Op_result;
template<typename Body, typename... As>
struct Op_result<Body, std::tuple<As...>>
{
    static constexpr bool takes_token = std::invocable<Body&, As..., const Cancellation_token&>;
    static constexpr bool invocable = std::invocable<Body&, As...> || takes_token;
    using type = typename Op_result_sel<invocable, takes_token, Body&, As...>::type;
};

// `Access_op<Args...>`'s compile-time shape. The pack is objects first, body last, so the
// split is "all but the last" and "the last"; every other property - each object's access
// mode, the references the body is invoked with, the result - follows from it.
template<typename Seq, typename... Args> struct Op_traits_impl;
template<std::size_t... I, typename... Args>
struct Op_traits_impl<std::index_sequence<I...>, Args...>
{
    using args = std::tuple<Args...>;
    static constexpr std::size_t arity = sizeof...(I);
    using body = std::tuple_element_t<arity, args>;
    using objects = std::tuple<typename Op_object<std::tuple_element_t<I, args>>::type...>;

    template<std::size_t P> using object = std::tuple_element_t<P, objects>;
    template<std::size_t P>
    static constexpr Access mode = Op_mode<body, P, std::tuple_element_t<P, args>, objects>::value;

    using refs = std::tuple<
        Mode_ref_t<Op_mode<body, I, std::tuple_element_t<I, args>, objects>::value, object<I>>...>;
    using result = typename Op_result<body, refs>::type;
    static constexpr bool takes_token = Op_result<body, refs>::takes_token;

    // The compile-time modes as a runtime array, in declaration order - what the op's binding
    // pass permutes into canonical order.
    static constexpr Access declared_modes[arity] = {
        Op_mode<body, I, std::tuple_element_t<I, args>, objects>::value... };
};

template<typename... Args>
using Op_traits = Op_traits_impl<std::make_index_sequence<sizeof...(Args) - 1>, Args...>;

// One object's binding for an `Access_op`, in declaration order: the pipe that serializes it
// and the address of the instance the body receives. The verbs build these (they can reach
// `Guarded`'s internals), so the op itself needs no `Guarded` at all.
struct Op_target
{
    Pipe* pipe;
    const void* inst;
};
}

// `Access_op<Objects..., Body>` - the caller-owned operation state the `access` verbs return
// (docs/access-op-design.md, docs/multi-access-op-design.md). `access` is the ATTENDED verb: the
// caller stays for the result, so the operation's whole state - completion core, result storage,
// body, one pipe entry per object - lives in the returned object instead of a heap block, and an
// access allocates nothing. `async` remains the detached verb and keeps returning a heap-backed
// `Task<R>`.
//
// The template argument list is objects first, body last, so one type serves every arity:
//
//   ts::Access_op<World, Snapshot>            // world.access(fn)
//   ts::Access_op<Combat, Economy, Hud>       // ts::access(fn, combat, economy)
//
// `Body` is the user's decayed functor, verbatim - no library wrapper closure - so the type is
// spellable for members: `ts::Access_op<World, Snapshot> op_;` with `Snapshot` a named functor.
// Each object's access mode and the result type are deduced from `Body` exactly as the verbs
// deduce them; a position tagged at the call site carries its mode in the type instead
// (`Access_op<detail::Access_arg<World, Access::read_only>, Fn>`) - a spelling the tagged verb's
// deduced return type produces, not one to write by hand.
//
// The op is eager (the constructor performs the access fast path: objects the calling task
// already holds are lent, the rest are admitted inline when every one of them is free, else
// enqueued through the canonical cascade) and pinned - non-copyable and non-movable, because the
// pipe's intrusive FIFO holds the embedded entries' addresses. Consume the result exactly once:
// `co_await op` from a coroutine, `op.sync()` from outside a task (returns `R` BY VALUE - the op
// owns the storage and often dies at the semicolon), `try_take()` for the non-blocking read, or
// `co_await op.as_optional()` for the cancellation-tolerant await. Destroying an unsettled op is a
// bug the destructor reports (`TS_ENSURE`) and then survives: it blocks until the access settles
// in every configuration - the caller-owned analog of the heap block's refcount.
template<typename... Args>
class Access_op
{
    static_assert(sizeof...(Args) >= 2,
        "Access_op names the guarded objects first and the body last: Access_op<World, Body>, "
        "Access_op<Combat, Economy, Body>");
    using Traits = detail::Op_traits<Args...>;

public:
    using Body = typename Traits::body;

    // Objects declared, hence pipe turns taken and `Access_context` entries made.
    static constexpr std::size_t arity = Traits::arity;
    static_assert(arity <= Access_context::max_entries,
        "an Access_op declares more objects than one grant context holds "
        "(Access_context::max_entries): access sets are coarse-grained by design - split the "
        "operation, or widen Access_context::max_entries");

    template<std::size_t I> using object_type = typename Traits::template object<I>;

    // The declared access mode of object `I`; `mode` is the first object's - the only one for
    // the single-object spelling, where it means exactly what it always did.
    template<std::size_t I> static constexpr Access mode_at = Traits::template mode<I>;
    static constexpr Access mode = mode_at<0>;

    using result_type = typename Traits::result;

    Access_op(const Access_op&) = delete;
    Access_op& operator=(const Access_op&) = delete;

    // Lifecycle (docs/access-op-design.md §10.1): unbound -> dormant -> in flight -> settled,
    // with `start()` the only verb that touches the pipe. The eager form (`world.access(fn)`)
    // is bind + start fused; these give members the deferred spellings. Single-owner: none of
    // start/bind/sync/destroy synchronize against each other.

    // Unbound: no target, no body - for members whose target does not exist yet. `bind()` then
    // `start()`.
    Access_op() = default;

    // Bound but dormant: target and body stored, pipe untouched; `start()` fires. Single-object
    // only - a multi-object op is bound by the verb that built it (a parameter pack cannot be
    // followed by the defaulted options and site these carry), and refires through `start()`.
    Access_op(Dormant, Guarded<object_type<0>>& target, Body body, Access_options opts = {},
              std::source_location site = std::source_location::current())
        requires (arity == 1);

    // Store the target + construct the body in place; does not fire. Legal on an unbound,
    // dormant, or settled op (rebinding destroys the old body; a settled op's pipe refs are
    // drained, so retargeting is safe - the pooled/reused-op enabler); fatal in flight.
    void bind(Guarded<object_type<0>>& target, Body body) requires (arity == 1);

    // Fire: first fire on a dormant op, refire on a settled one (an unconsumed result is
    // discarded - "skip a stale frame's read" is a legitimate steady state). Fatal on an
    // unbound op and while in flight. Zero-alloc in the steady state: the same storage
    // re-enters the pipe.
    void start();

    ~Access_op();

    bool is_done() const noexcept;
    bool is_cancelled() const noexcept;

    // The blocking consumes/peeks - one vocabulary with `Task`, applied to caller-owned
    // storage (legal only outside a task - the blue boundary; fatal on a cancelled value
    // access, check `is_cancelled()` first):
    //   sync() &   - non-consuming `const R&` peek, repeatable (`Task::sync`'s meaning)
    //   sync() &&  - `R` by value: the temporary form `obj.access(fn).sync()` stays
    //                dangle-free (the op dies at the semicolon)
    //   take()     - the explicit consuming move (`Task::take`'s meaning); consuming the
    //                same cycle twice is a checked fatal
    detail::Sync_ref_t<result_type> sync() &;
    result_type sync() &&;
    result_type take() requires (!std::is_void_v<result_type>);

    // Never blocks: empty when the access is unsettled or cancelled, else moves the result
    // out (the last consume). Also legal inside a task, like `Task::try_take`. Unlike `Task`
    // (which has no unstarted state), an `Access_op` can be unbound/dormant, and a
    // never-started op can never produce a result - polling it would return empty forever, so
    // that is a checked fatal, not a silent empty. The never-blocks contract is intact: a
    // started-but-not-ready op still returns immediately.
    std::optional<result_type> try_take() requires (!std::is_void_v<result_type>);

    // Awaitable-only: `co_await op.as_optional()` waits like `co_await op`, then yields empty
    // on cancellation instead of the fatal a bare await raises - the cancellation-tolerant
    // spelling for the verb most likely to carry a token. Moves the result out, so it is the
    // last consume of the cycle. Fatal on a never-started op, like `co_await` and `try_take`.
    detail::Optional_access_awaitable<result_type> as_optional() requires (!std::is_void_v<result_type>);

private:
    template<typename U> friend class Guarded;
    friend struct detail::Access_op_maker;
    template<typename... A> friend detail::Task_control_block* detail::access_op_core(Access_op<A...>&) noexcept;
    template<typename... A> friend bool* detail::access_op_consumed(Access_op<A...>&) noexcept;
    template<typename... A> friend bool detail::access_op_started(const Access_op<A...>&) noexcept;

    // The op's block: the monomorphic core as a base (recovery is the standard derived cast),
    // result storage, the flattened access context (instances/epochs/ranks as plain members -
    // what the heap path captures in a wrapper closure), the user's body, and one embedded pipe
    // entry per object. `caller_owned` custody: the machinery holds no ref on it, ever (Flags
    // doc).
    //
    // Two orderings, deliberately (docs/multi-access-op-design.md §3). The pipe arrays are in
    // CANONICAL (ascending pipe-address) order, because that is what makes the cascade
    // deadlock-free and it is the order graph nodes take their objects in. The instance arrays
    // are in DECLARATION order, because that is the order the body takes its parameters and the
    // order the compile-time modes (`mode_at<I>`) are indexed by. `decl_index` maps the first
    // onto the second.
    struct State : detail::Task_control_block
    {
        detail::Result_storage<result_type> storage;

        detail::Pipe_link links[arity];
        detail::Pipe* pipes[arity] = {};
        Access link_modes[arity] = {};
        std::uint8_t decl_index[arity] = {};

        const void* insts[arity] = {};
        const std::atomic<std::uint64_t>* epochs[arity] = {};
        unsigned ranks[arity] = {};

        // The body lives in raw storage behind the `bound` bit (symmetric with the result
        // storage): an unbound op has none, `bind()` constructs it in place, a rebind
        // destroys and reconstructs. No allocation, nothing on the fire path.
        alignas(Body) unsigned char body_store[sizeof(Body)];
        bool bound = false;
        bool started = false;    // ever fired; with `ready` it splits dormant/in flight/settled
        bool consumed = false;   // this cycle's result was moved out (take/sync&&/await/try_take)
        bool queued = false;     // `Access_options::queued`: skip the inline-when-free arm
        // Set by a fire around its inline/reentrant execute: no observer can exist before the
        // firing call returns (single owner, mid-call), so `op_settle` skips the mutex and the
        // notify entirely - plain stores, sequenced before every later member call.
        bool settle_synchronously = false;

        State();
        State(Body b, Cancellation_token tok, Priority pri, bool queued_opt);   // bound; targets set by the caller
        ~State();

        Body& body() noexcept { return *std::launder(reinterpret_cast<Body*>(body_store)); }

        // Object `I` as the body receives it: `const T&` for a read position, `T&` for a write
        // one (`mode_ref`), so a mutating body under a read classification does not compile.
        template<std::size_t I>
        detail::Mode_ref_t<mode_at<I>, object_type<I>> inst_ref() const noexcept
        {
            using Obj = object_type<I>;
            return detail::mode_ref<mode_at<I>>(const_cast<Obj*>(static_cast<const Obj*>(insts[I])));
        }

        static void run(const detail::Task_ptr& c);
        template<std::size_t... I>
        static void run_body(State* self, const detail::Task_ptr& c, std::index_sequence<I...>);
        static void settle_thunk(detail::Task_control_block* c);
        void op_settle(bool cancel);
        void finish(bool cancel);   // the shared completion tail: advance pipes, settle
    };

    Access_op(Body body, Access_options opts, Named name, const detail::Op_target (&targets)[arity]);

    // Store the objects: instances/epochs/ranks in declaration order, then the pipes sorted
    // into canonical order (a repeated object is fatal). Shared by every bind path.
    void bind_targets(const detail::Op_target (&targets)[arity]);

    // Bind the pipe entries this fire must take a turn on, canonically, and return how many.
    // Objects the calling task already holds are lent, not entered.
    std::uint8_t bind_links(bool lend);

    // The fire fast path (lend / inline when free / enqueue), shared by the eager constructor
    // and `start()`.
    void fire();

    // The blocking-wait prologue shared by `sync()`/`take()`: never-started check, the
    // in-task blocking-sync rule, then the settled fast path or the real wait.
    void wait_settled();

    State state_;
    // Set when the LAST fire's inline/reentrant arm ran the body to completion on this
    // thread: the settle fully preceded the firing call's return, so the destructor needs no
    // synchronization at all (the fast path pays no lock in the dtor).
    bool settled_sync_ = false;
};

template<typename... Args>
Access_op<Args...>::State::State()
{
    destroy = [](detail::Task_control_block*) {};   // caller-owned: a drained refcount frees nothing
    execute = &State::run;
    // The completion route for a nested-gated op: `release()`'s execution-flag branch calls
    // `on_complete` for a caller-owned block instead of the generic `complete()` (task_block.h).
    // Never fired by `op_settle`, so the seam is otherwise unused here.
    on_complete = &State::settle_thunk;
    flags.caller_owned = true;
    pipe_links = links;
}

template<typename... Args>
Access_op<Args...>::State::State(Body b, Cancellation_token tok, Priority pri, bool queued_opt)
    : State()
{
    ::new (static_cast<void*>(body_store)) Body(std::move(b));
    bound = true;
    token = std::move(tok);
    flags.priority = pri;
    queued = queued_opt;
}

template<typename... Args>
Access_op<Args...>::State::~State()
{
    if (bound)
        std::destroy_at(&body());   // a dormant body is a capability, not a pending effect - no lost-work check
}

template<typename... Args>
void Access_op<Args...>::State::run(const detail::Task_ptr& c)
{
    if (!c->claim())
        return;   // machinery bug (fatal under TS_SAFETY_CHECKS); skip in shipping
    auto* self = static_cast<State*>(c.get());
    if (c->token.is_cancel_requested() || c->prereq_cancelled.load(std::memory_order_acquire))
    {
        self->finish(true);
        return;
    }
    c->num_locks.store(detail::Task_control_block::execution_flag + 1, std::memory_order_relaxed);
    // Borrowed install, not a counted copy: the op provably outlives its own body, and the
    // slot is defused before `prev` is restored - no refcount traffic on the hot path. A
    // copy taken FROM the slot by body-launched machinery still counts normally; the one
    // copier that could outlive the op - `add_nested` - borrows the caller-owned parent the
    // same way (flag-first release, defuse without a dec; see `Flags::caller_owned`), so it
    // too holds no ref, and the op's destructor waits the nested run out.
    detail::Task_ptr prev = detail::Current_task::exchange_borrowed(c.get());
    run_body(self, c, std::make_index_sequence<arity>{});
    detail::Current_task::restore_borrowed(std::move(prev));   // defuses the borrowed install
    // The child set is frozen from here: `add_nested` requires the running task, and
    // `Current_task` is restored. If children are pending, this fire cannot settle
    // synchronously - clear the flag BEFORE dropping the self-lock, so a child's settle
    // (any thread, via `release()` -> `settle_thunk`) takes the locked settle path. The
    // grants stay held until then: the pipe advance lives in `finish`, which only the
    // completing release reaches - the coroutine-graph-node model.
    if (c->num_locks.load(std::memory_order_acquire) != detail::Task_control_block::execution_flag + 1)
        self->settle_synchronously = false;
    if (c->num_locks.fetch_sub(1, std::memory_order_acq_rel) == detail::Task_control_block::execution_flag + 1)
        self->finish(false);
    // else: the last nested child's settle completes the op through `settle_thunk`.
}

template<typename... Args>
template<std::size_t... I>
void Access_op<Args...>::State::run_body(State* self, const detail::Task_ptr& c, std::index_sequence<I...>)
{
    // The flattened access context: what the heap path's wrapper closure installs, done
    // structurally from the op's own members - every declared object, in declaration order.
    Access_context ctx;
    (ctx.add(self->insts[I], mode_at<I>, self->epochs[I], self->ranks[I]), ...);
    Access_scope scope(ctx);
    constexpr bool takes_token = Traits::takes_token;
    if constexpr (std::is_void_v<result_type>)
    {
        if constexpr (takes_token)
            detail::invoke_user_body(self->body(), self->template inst_ref<I>()..., c->token);
        else
            detail::invoke_user_body(self->body(), self->template inst_ref<I>()...);
    }
    else
    {
        // The result's move into storage is inside the seam with the call: the move that
        // lands the result in the optional is the body's own code, and a type whose move
        // allocates can throw there.
        detail::invoke_user_body([&]
        {
            if constexpr (takes_token)
                self->storage.result.emplace(self->body()(self->template inst_ref<I>()..., c->token));
            else
                self->storage.result.emplace(self->body()(self->template inst_ref<I>()...));
        });
        c->result_ptr = &*self->storage.result;
    }
}

template<typename... Args>
void Access_op<Args...>::State::settle_thunk(detail::Task_control_block* c)
{
    // A cancelled nested child is ordering-only (the flag is read before the body, which
    // already ran) - the op completes normally.
    static_cast<State*>(c)->finish(false);
}

template<typename... Args>
void Access_op<Args...>::State::finish(bool cancel)
{
    detail::advance_pipe_links(this);   // release the taken turn(s); admissions fire pipe-free
    op_settle(cancel);
}

// The op's settle. Two deliberate divergences from the generic `Task_control_block::settle`,
// both because the storage is caller-owned rather than refcounted: (1) the `done_cv` notify
// happens UNDER the mutex (the pipe's `idle` teardown pattern) - a woken `sync()` waiter
// cannot return until it re-acquires the mutex, so it cannot destroy the op while this
// thread is still inside `notify_all`; (2) nothing on this frame touches a member after the
// continuations fire - a fired continuation can resume the awaiting coroutine, whose frame
// owns this op, and run it to the end of the `co_await`'s full-expression, destroying the op
// (and possibly the frame) before the loop below even advances.
template<typename... Args>
void Access_op<Args...>::State::op_settle(bool cancel)
{
    if (settle_synchronously)
    {
        // In-constructor settle (inline / reentrant arm): the op has not been returned to
        // its owner, so no waiter and no continuation can exist - plain stores suffice.
        completed = true;
        cancelled = cancel;
        ready.store(true, std::memory_order_release);
        return;
    }
    std::vector<std::move_only_function<void(void*, bool)>> conts;
    void* r = nullptr;
    {
        std::scoped_lock lock(mutex);
        completed = true;
        cancelled = cancel;
        ready.store(true, std::memory_order_release);
        conts = std::move(continuations);
        r = cancel ? nullptr : result_ptr;
        done_cv.notify_all();
    }
    for (auto& cont : conts)
        cont(r, cancel);
}

template<typename... Args>
void Access_op<Args...>::bind_targets(const detail::Op_target (&targets)[arity])
{
    constexpr auto& modes = Traits::declared_modes;
    // Declaration order: what the body is invoked with, and what the access context declares.
    for (std::size_t k = 0; k < arity; ++k)
    {
        state_.insts[k] = targets[k].inst;
        state_.epochs[k] = detail::pipe_epoch(*targets[k].pipe);
        state_.ranks[k] = detail::pipe_rank(*targets[k].pipe);
    }
    // Canonical order: insertion sort by pipe address (the pack is small), carrying each
    // pipe's mode and its declaration slot. The same globally canonical order the graph's
    // nodes and the multi-object `async` cascade use, which is what makes a cross-object wait
    // cycle unrepresentable. A repeated object is fatal: declare each object once, with the
    // strongest mode the body needs.
    std::size_t n = 0;
    for (std::size_t k = 0; k < arity; ++k)
    {
        detail::Pipe* pk = targets[k].pipe;
        std::size_t i = 0;
        while (i < n && state_.pipes[i] < pk)
            ++i;
        if (i < n && state_.pipes[i] == pk)
        {
            ts::fatal("ts::access/ts::async: the same Guarded object was passed twice - declare "
                      "each object once, with the strongest access the body needs");
        }
        for (std::size_t j = n; j > i; --j)
        {
            state_.pipes[j] = state_.pipes[j - 1];
            state_.link_modes[j] = state_.link_modes[j - 1];
            state_.decl_index[j] = state_.decl_index[j - 1];
        }
        state_.pipes[i] = pk;
        state_.link_modes[i] = modes[k];
        state_.decl_index[i] = static_cast<std::uint8_t>(k);
        ++n;
    }
}

template<typename... Args>
Access_op<Args...>::Access_op(Body body, Access_options opts, Named name,
                              const detail::Op_target (&targets)[arity])
    : state_(std::move(body), std::move(opts.token), detail::resolved_priority(opts.priority), opts.queued)
{
    bind_targets(targets);
    {
        detail::Task_ptr self(&state_, detail::Adopt_ref{});   // borrowed wrapper, defused below
        detail::set_task_name(self, name);
        self.release();
    }
    state_.started = true;
    fire();
}

// The lend protocol (docs/multi-access-op-design.md §7.2), the same one a nested graph run uses
// (`Static_task_graph::bind_links_for_run`): ask the calling task's `Access_context` - keyed by
// instance address - whether it already holds a grant covering what this object needs. It does
// exactly when the op runs inside that grant's window, which is already the exclusion the
// object needs, so the op takes no turn on it; taking one would only queue the op behind the
// caller's own hold, and awaiting it from there is a deadlock. Lending the single-object write
// case is what used to be the separate reentrant arm.
template<typename... Args>
std::uint8_t Access_op<Args...>::bind_links(bool lend)
{
    // The context read is the one thread-local touch on the fire path, and it sits behind the
    // out-of-line barrier (`access_load`), so it costs a call. `fire` therefore binds without
    // lending first and probes: when every object is free - the common case - the grant
    // question is never asked. Lending is consulted only after that probe fails, which loses
    // nothing: an object the caller holds for writing is never admissible, so the probe fails
    // on it and the lend happens on the retry; one the caller holds for reading admits a
    // second reader, which is the single-object behaviour exactly; and the read-holder-writes
    // fatal still fires, because that probe can never succeed.
    const Access_context* ctx = lend ? detail::access_load() : nullptr;
    std::uint8_t bound = 0;
    for (std::size_t i = 0; i < arity; ++i)
    {
        const Access mode_i = state_.link_modes[i];
        if (ctx != nullptr)
        {
            const void* inst = state_.insts[state_.decl_index[i]];
            if (ctx->grants(inst, mode_i))
                continue;   // lent: contained in the caller's grant window
#if TS_SAFETY_CHECKS
            if (mode_i == Access::read_write && ctx->grants(inst, Access::read_only))
            {
                // Mode-incompatible overlap, exactly the graph's nested-run case: a read grant
                // cannot be lent to a writer, and queueing would put the op behind the caller's
                // own read hold, which the caller cannot release while it waits for the op.
                ts::fatal("ts::access - the calling task holds READ access on an object this access "
                          "writes; a read grant cannot be lent to a writer (declare the write on the "
                          "calling task, or hand the write to ts::async and do not wait for it)");
            }
#endif
        }
        detail::bind_pipe_link(&state_, bound, *state_.pipes[i], mode_i);
        ++bound;
    }
    state_.pipe_count = bound;   // `bind_pipe_link` cannot say this when nothing was bound
    // An all-lent fire is indistinguishable from a bare task by the links alone (both have no
    // pipes), and the guard-across-suspension rule's one exemption is exactly this shape.
    state_.flags.all_lent = (bound == 0);
    return bound;
}

template<typename... Args>
void Access_op<Args...>::fire()
{
    settled_sync_ = false;
    // Borrowed wrapper for this frame's machinery calls - defused before every return
    // (`this` outlives the call by the single-owner contract; the refcount stays untouched).
    detail::Task_ptr self(&state_, detail::Adopt_ref{});
    // Inline fast path first, over every object and with no grant lookup: claim every pipe at
    // once, all or nothing, and run on this thread. `.queued` skips this arm (never-inline is
    // a dispatch preference). A failed probe admits nothing, so the links can be rebound below.
    if (!state_.queued)
    {
        const std::uint8_t bound = bind_links(false);
        state_.num_locks.store(bound, std::memory_order_relaxed);
        state_.settle_synchronously = true;
        if (detail::pipe_try_inline(self))
        {
            settled_sync_ = state_.settle_synchronously;   // false if the body attached children
            self.release();
            return;
        }
        state_.settle_synchronously = false;
    }
    // Something is busy (or never-inline was asked for). Now ask whether the caller is what
    // holds it: a lent object takes no turn, which is correctness rather than opportunism - an
    // op queued behind its own caller's held grant deadlocks when awaited.
    const std::uint8_t bound = bind_links(true);
    if (bound == 0)
    {
        // Every object lent: nothing to acquire, so the body runs inline under the grants the
        // caller already holds, touching no pipe at all. `.queued` does not apply here.
        state_.settle_synchronously = true;
        state_.execute(self);
        // `run` cleared the flag if the body attached nested children - the op is then still
        // in flight when this call returns, and the destructor must synchronize.
        settled_sync_ = state_.settle_synchronously;
        self.release();
        return;
    }
    state_.num_locks.store(bound, std::memory_order_relaxed);   // one per pipe turn taken
    // Lending narrowed the set, so the objects that made the first probe fail may be exactly
    // the lent ones. Probe what remains: a caller holding `a` with `b` free runs inline on `b`,
    // the same outcome lending-first would have produced - the first probe only deferred the
    // grant question, it did not forfeit the inline arm. Still never-inline under `.queued`.
    if (!state_.queued && bound < arity)
    {
        state_.settle_synchronously = true;
        if (detail::pipe_try_inline(self))
        {
            settled_sync_ = state_.settle_synchronously;
            self.release();
            return;
        }
        state_.settle_synchronously = false;
    }
    // Enqueue: the last admitted turn's release dispatches the body (borrowed route, no
    // machinery refs).
    self.release();
    detail::pipe_enter_first(&state_, nullptr);   // turns cascade canonically; the last release dispatches
}

template<typename... Args>
void Access_op<Args...>::start()
{
#if TS_SAFETY_CHECKS
    if (!state_.bound)
        ts::fatal("Access_op::start() on an unbound op - bind() a target and body first");
    if (state_.started && !state_.ready.load(std::memory_order_acquire))
        ts::fatal("Access_op::start() while the access is in flight - consume it first "
                  "(start() fires a dormant op or refires a settled one)");
#endif
    if (state_.started)
    {
        // Re-arm the settled core for the next cycle. The brief mutex pass serializes past
        // the settling thread's tail (a lock-free `ready` read can observe a settle
        // mid-flight); everything else is single-owner plain state.
        {
            std::scoped_lock lock(state_.mutex);
            state_.completed = false;
            state_.cancelled = false;
            state_.ready.store(false, std::memory_order_relaxed);
        }
        state_.body_claimed.store(false, std::memory_order_relaxed);
        state_.prereq_cancelled.store(false, std::memory_order_relaxed);
        state_.num_locks.store(0, std::memory_order_relaxed);
        state_.pipes_entered = 0;   // a reentrant refire must not re-advance the last cycle's turn
        if constexpr (!std::is_void_v<result_type>)
        {
            state_.storage.result.reset();   // discard an unconsumed (or moved-from) result
            state_.result_ptr = nullptr;
        }
        state_.consumed = false;
    }
    state_.started = true;
    fire();
}

template<typename... Args>
Access_op<Args...>::~Access_op()
{
    // Unbound / dormant: no fire outstanding, nothing to synchronize (the State dtor
    // destroys a bound body). Settled-synchronously: the settle preceded the firing call's
    // return on this very thread.
    if (!state_.started || settled_sync_)
        return;
    if (!state_.ready.load(std::memory_order_acquire))
    {
#if TS_RULE_ON(TS_RULE_IN_TASK_SYNC)
        // In-flight at destruction INSIDE a task: the wait below is a blocking sync, and the
        // blue boundary applies to it exactly as to `sync()` - fatal, with the same sharp
        // same-object diagnosis.
        if (detail::Current_task::get() != nullptr && rule_enforced(Rule::in_task_sync))
            detail::blocking_sync_diagnose(&state_);
#endif
        TS_ENSURE(false,
            "Access_op destroyed before completion - co_await or sync() it first (the destructor "
            "blocks until the access settles)");
        detail::drain_serial_pending();
    }
    // Wait until settled - and, via the mutex, until the settling thread is past every
    // member touch (`op_settle` notifies under the lock). Runs even when `ready` already
    // reads true: the lock-free read can observe the settle mid-flight.
    state_.wait();
}

template<typename... Args>
bool Access_op<Args...>::is_done() const noexcept
{
    return state_.ready.load(std::memory_order_acquire);
}

template<typename... Args>
bool Access_op<Args...>::is_cancelled() const noexcept
{
    return state_.ready.load(std::memory_order_acquire) && state_.cancelled;
}

template<typename... Args>
void Access_op<Args...>::wait_settled()
{
#if TS_SAFETY_CHECKS
    if (!state_.started)
        ts::fatal("Access_op: waiting on an op that was never started - start() it first "
                  "(waiting on a dormant op would hang forever)");
#endif
#if TS_RULE_ON(TS_RULE_IN_TASK_SYNC)
    // The rule is about the call, not the incident (as `sync_wait`): checked before the
    // settled fast path, so an in-task wait faults deterministically even on a settled op.
    if (detail::Current_task::get() != nullptr && rule_enforced(Rule::in_task_sync))
        detail::blocking_sync_diagnose(&state_);
#endif
    if (!state_.ready.load(std::memory_order_acquire))
    {
        detail::Task_ptr self(&state_, detail::Adopt_ref{});
        detail::Task_control_block::sync_wait(self);
        self.release();
    }
}

template<typename... Args>
detail::Sync_ref_t<typename Access_op<Args...>::result_type> Access_op<Args...>::sync() &
{
    wait_settled();
    if constexpr (std::is_void_v<result_type>)
    {
        return;
    }
    else
    {
        if (state_.cancelled)
            ts::fatal("Access_op::sync() on a cancelled access; check is_cancelled() first");
        return *static_cast<const result_type*>(state_.result_ptr);
    }
}

template<typename... Args>
typename Access_op<Args...>::result_type Access_op<Args...>::sync() &&
{
    if constexpr (std::is_void_v<result_type>)
        wait_settled();
    else
        return take();   // the temporary form is a consume; same contract, same fatals
}

template<typename... Args>
typename Access_op<Args...>::result_type Access_op<Args...>::take()
    requires (!std::is_void_v<result_type>)
{
    wait_settled();
    if (state_.cancelled)
        ts::fatal("Access_op::take() on a cancelled access; check is_cancelled() first");
#if TS_SAFETY_CHECKS
    if (state_.consumed)
        ts::fatal("Access_op: result already consumed this cycle - start() refires before "
                  "the next consume");
#endif
    state_.consumed = true;
    return std::move(*static_cast<result_type*>(state_.result_ptr));
}

template<typename... Args>
std::optional<typename Access_op<Args...>::result_type> Access_op<Args...>::try_take()
    requires (!std::is_void_v<result_type>)
{
#if TS_SAFETY_CHECKS
    if (!state_.started)
        ts::fatal("Access_op::try_take() on an op that was never started - start() it first "
                  "(a never-started op produces no result; try_take would poll empty forever)");
#endif
    if (!state_.ready.load(std::memory_order_acquire) || state_.cancelled || state_.consumed)
        return std::nullopt;
    state_.consumed = true;
    return std::move(*static_cast<result_type*>(state_.result_ptr));
}

template<typename... Args>
detail::Optional_access_awaitable<typename Access_op<Args...>::result_type>
Access_op<Args...>::as_optional() requires (!std::is_void_v<result_type>)
{
#if TS_SAFETY_CHECKS
    if (!state_.started)
        ts::fatal("Access_op::as_optional() on an op that was never started - start() it first "
                  "(awaiting a dormant op would suspend forever)");
#endif
    return detail::Optional_access_awaitable<result_type>{ detail::Task_ptr(&state_), &state_.consumed };
}

namespace detail
{
template<typename... Args>
Task_control_block* access_op_core(Access_op<Args...>& op) noexcept
{
    return &op.state_;
}

template<typename... Args>
bool* access_op_consumed(Access_op<Args...>& op) noexcept
{
    return &op.state_.consumed;
}

template<typename... Args>
bool access_op_started(const Access_op<Args...>& op) noexcept
{
    return op.state_.started;
}

// The single-object spelling `Access_op<T, Body>` must keep meaning exactly what it meant
// before the type became variadic (docs/multi-access-op-design.md §7.1) - enforced here rather
// than asserted in prose: one object, the mode `accessor_mode` deduces, and the result
// `Accessor_result_t` computes under it.
template<typename T, typename Body>
inline constexpr bool op_matches_single_object_deduction =
    Access_op<T, Body>::arity == 1
    && Access_op<T, Body>::mode == accessor_mode<Body, T>()
    && std::is_same_v<typename Access_op<T, Body>::result_type,
                      Accessor_result_t<Body, T, accessor_mode<Body, T>()>>;

// Declarations only - the probes are never called, just classified.
struct Op_compat_read { int operator()(const int&) const; };
struct Op_compat_write { void operator()(int&) const; };
struct Op_compat_token { int operator()(const int&, const Cancellation_token&) const; };
struct Op_compat_generic { template<typename V> void operator()(V&) const; };

static_assert(op_matches_single_object_deduction<int, Op_compat_read>);
static_assert(op_matches_single_object_deduction<int, Op_compat_write>);
static_assert(op_matches_single_object_deduction<int, Op_compat_token>);
static_assert(op_matches_single_object_deduction<int, Op_compat_generic>);
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
// `access` takes `Access_options` (task.h) = `{token, priority, name, queued}` and `async`
// takes `Dispatch_options` = the same minus `queued`: the verb picks inline vs enqueued, and
// only `access` has an inline arm to opt out of (`async` is enqueued by definition).
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
    // `Access_options` = `{token, priority, name, queued}` on `access`, `Dispatch_options` =
    // the same minus `queued` on `async`, which has no inline arm to skip.
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

    // access, read_write. Returns the caller-owned `Access_op` (zero-alloc; see the class
    // doc): consume via `co_await`, `.sync()` (from blue), or `try_take()`. To detach - drop
    // the handle, store a `Task<R>` - use `async`.
    template<typename Fn>
        requires detail::Read_write_accessor<Fn, T>
    [[nodiscard("the attended verb: consume the op (co_await, .sync(), try_take()). To not wait, use async")]]
    auto access(Fn&& fn, Access_options opts = {},
                std::source_location site = std::source_location::current())
        -> Access_op<T, std::decay_t<Fn>>
    {
        const detail::Op_target targets[1] = { { &pipe_, &instance_ } };
        return Access_op<T, std::decay_t<Fn>>(std::forward<Fn>(fn), opts,
                                              detail::named_from(opts, site), targets);
    }

    // access, read_only.
    template<typename Fn>
        requires detail::Read_only_accessor<Fn, T>
    [[nodiscard("the attended verb: consume the op (co_await, .sync(), try_take()). To not wait, use async")]]
    auto access(Fn&& fn, Access_options opts = {},
                std::source_location site = std::source_location::current()) const
        -> Access_op<T, std::decay_t<Fn>>
    {
        const detail::Op_target targets[1] = { { &pipe_, &instance_ } };
        return Access_op<T, std::decay_t<Fn>>(std::forward<Fn>(fn), opts,
                                              detail::named_from(opts, site), targets);
    }

    // async, read_write: always enqueued (never inline).
    template<typename Fn>
        requires detail::Read_write_accessor<Fn, T>
    auto async(Fn&& fn, Dispatch_options opts = {},
               std::source_location site = std::source_location::current())
        -> Task<detail::Accessor_result_t<Fn, T, Access::read_write>>
    {
        return launch<detail::Accessor_result_t<Fn, T, Access::read_write>, Access::read_write>(
            &instance_, std::forward<Fn>(fn), opts, detail::named_from(opts, site));
    }

    // async, read_only: always enqueued (never inline).
    template<typename Fn>
        requires detail::Read_only_accessor<Fn, T>
    auto async(Fn&& fn, Dispatch_options opts = {},
               std::source_location site = std::source_location::current()) const
        -> Task<detail::Accessor_result_t<Fn, T, Access::read_only>>
    {
        return launch<detail::Accessor_result_t<Fn, T, Access::read_only>, Access::read_only>(
            &instance_, std::forward<Fn>(fn), opts, detail::named_from(opts, site));
    }

private:
    // The heap-block path behind `async` (and `Deferred::commit`'s recorded write): always
    // enqueued, detached custody. `access`'s inline/reentrant arms live on `Access_op`.
    template<typename R, Access mode, typename Inst, typename Fn>
    Task<R> launch(Inst* inst, Fn&& fn, Dispatch_options opts, Named name,
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
        core->flags.priority = detail::resolved_priority(opts.priority);
        detail::set_task_name(core, name);
        detail::bind_pipe_link(core.get(), 0, pipe_, mode);
        core->num_locks.store(1, std::memory_order_relaxed);   // the one pipe turn
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
    static Task<void> commit_write(Guarded<T>& t, Fn&& fn, Dispatch_options opts, Named name, Task_ptr* record)
    {
        return t.template launch<void, Access::read_write>(
            &t.instance_, std::forward<Fn>(fn), opts, name, record);
    }
};

} // namespace detail

// Defined here rather than with the other `Access_op` members: both need `Guarded`'s
// internals (`Guarded_access`), which are only complete below the class.
template<typename... Args>
Access_op<Args...>::Access_op(Dormant, Guarded<object_type<0>>& target, Body body,
                              Access_options opts, std::source_location site)
    requires (arity == 1)
    : state_(std::move(body), std::move(opts.token), detail::resolved_priority(opts.priority), opts.queued)
{
    const detail::Op_target targets[1] = {
        { &detail::Guarded_access::pipe(target), detail::Guarded_access::instance(target) } };
    bind_targets(targets);
    {
        detail::Task_ptr self(&state_, detail::Adopt_ref{});
        detail::set_task_name(self, detail::named_from(opts, site));
        self.release();
    }
}

template<typename... Args>
void Access_op<Args...>::bind(Guarded<object_type<0>>& target, Body body) requires (arity == 1)
{
#if TS_SAFETY_CHECKS
    if (state_.started && !state_.ready.load(std::memory_order_acquire))
        ts::fatal("Access_op::bind() while the access is in flight - the queued entry still "
                  "references the current target");
#endif
    if (state_.bound)
        std::destroy_at(&state_.body());
    ::new (static_cast<void*>(state_.body_store)) Body(std::move(body));
    const detail::Op_target targets[1] = {
        { &detail::Guarded_access::pipe(target), detail::Guarded_access::instance(target) } };
    bind_targets(targets);
    state_.bound = true;
}

namespace detail
{

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
auto async_build_modes(Dispatch_options opts, std::index_sequence<I...>, Fn&& fn, Guarded<Ts>&... objs)
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
    block->flags.priority = resolved_priority(opts.priority);
    // Multi-object `ts::access`/`ts::async` end in an object pack, so no trailing defaulted
    // `source_location` is expressible and there is no call site for the verb to capture:
    // `Dispatch_options::name` is the whole identity, which is why it is a `Named` - spell
    // `{.name = "hud"}` for a literal or `{.name = ts::Named{}}` to capture the call site.
    set_task_name(block, opts.name);
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
auto async_build(Dispatch_options opts, std::index_sequence<I...> seq, Fn&& fn, Guarded<Ts>&... objs)
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
auto async_build_probed(Dispatch_options opts, std::index_sequence<I...> seq, Fn&& fn, Guarded<Ts>&... objs)
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
auto async_build_tagged(Dispatch_options opts, std::index_sequence<I...> seq, Fn&& fn, Objs&&... objs)
{
    return async_build_modes<std::remove_cvref_t<Objs>::mode...>(
        std::move(opts), seq, std::forward<Fn>(fn), *objs.obj...);
}

// Construct an `Access_op` in the caller's storage: flatten the objects into targets (pipe +
// instance, declaration order) and hand them to the op's private constructor. Every prvalue
// on the way out is elided, which is what lets a non-movable op be built by a verb.
struct Access_op_maker
{
    template<typename Op, typename Fn, typename... Ts>
    static Op make(Access_options opts, Fn&& fn, Guarded<Ts>&... objs)
    {
        static_assert(sizeof...(Ts) == Op::arity,
            "the op's object arguments and the verb's object pack must agree in count");
        const Op_target targets[sizeof...(Ts)] = {
            { &Guarded_access::pipe(objs), static_cast<const void*>(Guarded_access::instance(objs)) }... };
        Named name = opts.name;
        return Op(std::forward<Fn>(fn), std::move(opts), name, targets);
    }
};

// The `access` counterpart of the `async_build_*` trio: the same three tiers over the same
// canonical cascade, with the operation state in the CALLER's storage instead of a heap block.
// The modes are not threaded through - the op recomputes them from its own template arguments,
// which is why a tagged position spells `Access_arg<T, M>` in the op's type and a bare one
// spells `T`.
template<typename Args, std::size_t... I, typename Fn, typename... Ts>
auto access_build(Access_options opts, std::index_sequence<I...>, Fn&& fn, Guarded<Ts>&... objs)
{
    static_assert((std::is_lvalue_reference_v<std::tuple_element_t<I, Args>> && ...),
        "a guarded-resource parameter must be `T&` or `const T&`: taking it by value copies "
        "the resource (writes hit the copy and are silently discarded), and `T&&` cannot "
        "bind the stored instance");
    return Access_op_maker::make<Access_op<Ts..., std::decay_t<Fn>>>(
        std::move(opts), std::forward<Fn>(fn), objs...);
}

template<std::size_t... I, typename Fn, typename... Ts>
auto access_build_probed(Access_options opts, std::index_sequence<I...>, Fn&& fn, Guarded<Ts>&... objs)
{
    static_assert(std::invocable<Fn, Ts&...>,
        "multi-object access/async: functor parameters must match the Guarded objects "
        "(same arity, each taken by reference)");
    // Guard the forward on the same condition: a failed assert does not stop instantiation,
    // so without it the op's own deduction re-errors past the message.
    if constexpr (std::invocable<Fn, Ts&...>)
    {
        return Access_op_maker::make<Access_op<Ts..., std::decay_t<Fn>>>(
            std::move(opts), std::forward<Fn>(fn), objs...);
    }
}

template<std::size_t... I, typename Fn, typename... Objs>
auto access_build_tagged(Access_options opts, std::index_sequence<I...>, Fn&& fn, Objs&&... objs)
{
    return Access_op_maker::make<Access_op<std::remove_cvref_t<Objs>..., std::decay_t<Fn>>>(
        std::move(opts), std::forward<Fn>(fn), *objs.obj...);
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
auto async(Dispatch_options opts, Fn&& fn, Objs&&... objs)
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
    return async(Dispatch_options{}, std::forward<Fn>(fn), std::forward<Objs>(objs)...);
}

// Multi-object `access`: the attended sibling of `ts::async(fn, objs...)`, taking the same
// bare-or-tagged arguments and the same deadlock-free canonical order. Returns the caller-owned
// `Access_op<Objects..., Body>` - the operation state lives in the returned object, so a
// multi-object access allocates nothing; consume it with `co_await`, `.sync()` (from outside a
// task) or `try_take()`. Dispatch is opportunistic: objects the calling task already holds are
// lent (no turn taken at all), and the rest run inline on the calling thread when every one of
// them is free right now, otherwise the whole set enqueues through the cascade. To detach -
// drop the handle, store a `Task<R>` - use `ts::async`.
template<typename Fn, typename... Objs>
    requires (sizeof...(Objs) >= 1) && (detail::Object_arg<Objs> && ...)
[[nodiscard("the attended verb: consume the op (co_await, .sync(), try_take()). To not wait, use ts::async")]]
auto access(Access_options opts, Fn&& fn, Objs&&... objs)
{
    constexpr bool any_tagged = (detail::is_access_arg_v<Objs> || ...);
    if constexpr (any_tagged)
    {
        static_assert((detail::is_access_arg_v<Objs> && ...),
            "multi-object access: don't mix tagged (ts::as_read_only/as_read_write) and bare Guarded "
            "arguments - tag EVERY object argument, or tag none");
        return detail::access_build_tagged(std::move(opts), std::index_sequence_for<Objs...>{},
            std::forward<Fn>(fn), std::forward<Objs>(objs)...);
    }
    else if constexpr (detail::introspectable_v<Fn>)
    {
        using Args = typename detail::Function_traits<std::decay_t<Fn>>::args;
        static_assert(std::tuple_size_v<Args> == sizeof...(Objs),
            "multi-object access: functor arity must match the number of Guarded objects");
        return detail::access_build<Args>(std::move(opts), std::index_sequence_for<Objs...>{},
            std::forward<Fn>(fn), objs...);
    }
    else
    {
        return detail::access_build_probed(std::move(opts), std::index_sequence_for<Objs...>{},
            std::forward<Fn>(fn), objs...);
    }
}

template<typename Fn, typename... Objs>
    requires (sizeof...(Objs) >= 1) && (detail::Object_arg<Objs> && ...)
[[nodiscard("the attended verb: consume the op (co_await, .sync(), try_take()). To not wait, use ts::async")]]
auto access(Fn&& fn, Objs&&... objs)
{
    return access(Access_options{}, std::forward<Fn>(fn), std::forward<Objs>(objs)...);
}

// `ts::launch` (bare scheduler task) lives in task.h now - it dispatches
// through the `submit_ready` bridge and touch no `Guarded`, so they belong with the task core.

} // namespace ts
