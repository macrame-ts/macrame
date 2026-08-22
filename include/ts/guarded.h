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

template<typename T> class Guarded;
template<typename T, typename Body> class Access_op;

// Tag for the bound-but-dormant `Access_op` constructor: `Access_op(ts::dormant, world, body)`
// stores the target and body without firing - `start()` fires later. Needed because a member
// init list cannot call `bind()`. (Named `dormant`, not `defer` - `Deferred` is taken.)
struct Dormant {};
inline constexpr Dormant dormant{};

namespace detail
{
// `Access_op::sync() &`'s return: a non-consuming `const R&` peek, `void` for a void access.
template<typename R2> struct Sync_ref { using type = const R2&; };
template<> struct Sync_ref<void> { using type = void; };
template<typename R2> using Sync_ref_t = typename Sync_ref<R2>::type;

// The op's hooks for the coroutine awaiter (coroutine_support.h). Defined below the class.
template<typename T, typename Body>
Task_control_block* access_op_core(Access_op<T, Body>& op) noexcept;
template<typename T, typename Body>
bool* access_op_consumed(Access_op<T, Body>& op) noexcept;
template<typename T, typename Body>
bool access_op_started(const Access_op<T, Body>& op) noexcept;
}

// `Access_op<T, Body>` - the caller-owned operation state `Guarded<T>::access` returns
// (docs/access-op-design.md). `access` is the ATTENDED verb: the caller stays for the result, so
// the operation's whole state - completion core, result storage, body, pipe entry - lives in the
// returned object instead of a heap block, and an access allocates nothing. `async` remains the
// detached verb and keeps returning a heap-backed `Task<R>`.
//
// `Body` is the user's decayed functor, verbatim - no library wrapper closure - so the type is
// spellable for members: `ts::Access_op<World, Snapshot> op_;` with `Snapshot` a named functor.
// The access mode and result type are deduced from `Body` exactly as the verb deduces them.
//
// The op is eager (the constructor performs the access fast path: reentrant arm, inline when the
// pipe is free, else enqueued) and pinned - non-copyable and non-movable, because the pipe's
// intrusive FIFO holds the embedded entry's address. Consume the result exactly once:
// `co_await op` from a coroutine, `op.sync()` from outside a task (returns `R` BY VALUE - the op
// owns the storage and often dies at the semicolon), or `try_take()` for the non-blocking /
// cancellation-tolerant read. Destroying an unsettled op is a bug the destructor reports
// (`TS_ENSURE`) and then survives: it blocks until the access settles in every configuration -
// the caller-owned analog of the heap block's refcount.
template<typename T, typename Body>
class Access_op
{
public:
    static constexpr Access mode = detail::accessor_mode<Body, T>();
    using result_type = detail::Accessor_result_t<Body, T, mode>;

    Access_op(const Access_op&) = delete;
    Access_op& operator=(const Access_op&) = delete;

    // Lifecycle (docs/access-op-design.md §10.1): unbound -> dormant -> in flight -> settled,
    // with `start()` the only verb that touches the pipe. The eager form (`world.access(fn)`)
    // is bind + start fused; these give members the deferred spellings. Single-owner: none of
    // start/bind/sync/destroy synchronize against each other.

    // Unbound: no target, no body - for members whose target does not exist yet. `bind()` then
    // `start()`.
    Access_op() = default;

    // Bound but dormant: target and body stored, pipe untouched; `start()` fires.
    Access_op(Dormant, Guarded<T>& target, Body body, Access_options opts = {},
              std::source_location site = std::source_location::current());

    // Store the target + construct the body in place; does not fire. Legal on an unbound,
    // dormant, or settled op (rebinding destroys the old body; a settled op's pipe refs are
    // drained, so retargeting is safe - the pooled/reused-op enabler); fatal in flight.
    void bind(Guarded<T>& target, Body body);

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

private:
    template<typename U> friend class Guarded;
    template<typename U, typename B> friend detail::Task_control_block* detail::access_op_core(Access_op<U, B>&) noexcept;
    template<typename U, typename B> friend bool* detail::access_op_consumed(Access_op<U, B>&) noexcept;
    template<typename U, typename B> friend bool detail::access_op_started(const Access_op<U, B>&) noexcept;

    // A read access stores (and hands the body) `const T*`; the const-ness is structural, not
    // a runtime mode check.
    using Inst = std::conditional_t<mode == Access::read_only, const T, T>;

    // The op's block: the monomorphic core as a base (recovery is the standard derived cast),
    // result storage, the flattened access context (instance/epoch/rank as plain members - what
    // the heap path captures in a wrapper closure), the user's body, and the embedded pipe
    // entry. `caller_owned` custody: the machinery holds no ref on it, ever (Flags doc).
    struct State : detail::Task_control_block
    {
        detail::Result_storage<result_type> storage;
        detail::Pipe_link link;
        detail::Pipe* pipe = nullptr;
        Inst* inst = nullptr;
        const std::atomic<std::uint64_t>* epoch = nullptr;
        unsigned rank = 0;
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
        State(Body b, Cancellation_token tok, Priority pri, bool queued_opt);   // bound; target set by the caller
        ~State();

        Body& body() noexcept { return *std::launder(reinterpret_cast<Body*>(body_store)); }

        static void run(const detail::Task_ptr& c);
        static void settle_thunk(detail::Task_control_block* c);
        void op_settle(bool cancel);
        void finish(bool cancel);   // the shared completion tail: advance pipes, settle
    };

    Access_op(detail::Pipe& pipe, Inst* inst, Body body, Access_options opts, Named name);

    // The fire fast path (reentrant arm / inline when free / enqueue), shared by the eager
    // constructor and `start()`.
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

template<typename T, typename Body>
Access_op<T, Body>::State::State()
{
    destroy = [](detail::Task_control_block*) {};   // caller-owned: a drained refcount frees nothing
    execute = &State::run;
    // The completion route for a nested-gated op: `release()`'s execution-flag branch calls
    // `on_complete` for a caller-owned block instead of the generic `complete()` (task_block.h).
    // Never fired by `op_settle`, so the seam is otherwise unused here.
    on_complete = &State::settle_thunk;
    flags.caller_owned = true;
    pipe_links = &link;
}

template<typename T, typename Body>
Access_op<T, Body>::State::State(Body b, Cancellation_token tok, Priority pri, bool queued_opt)
    : State()
{
    ::new (static_cast<void*>(body_store)) Body(std::move(b));
    bound = true;
    token = std::move(tok);
    flags.priority = pri;
    queued = queued_opt;
}

template<typename T, typename Body>
Access_op<T, Body>::State::~State()
{
    if (bound)
        std::destroy_at(&body());   // a dormant body is a capability, not a pending effect - no lost-work check
}

template<typename T, typename Body>
void Access_op<T, Body>::State::run(const detail::Task_ptr& c)
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
    {
        // The flattened access context: what the heap path's wrapper closure installs, done
        // structurally from the op's own members.
        Access_context ctx;
        ctx.add(static_cast<const void*>(self->inst), mode, self->epoch, self->rank);
        Access_scope scope(ctx);
        constexpr bool takes_token = detail::accessor_takes_token_v<Body, Inst&>;
        if constexpr (std::is_void_v<result_type>)
        {
            if constexpr (takes_token)
                detail::invoke_user_body(self->body(), *self->inst, c->token);
            else
                detail::invoke_user_body(self->body(), *self->inst);
        }
        else
        {
            if constexpr (takes_token)
                self->storage.result.emplace(detail::invoke_user_body(self->body(), *self->inst, c->token));
            else
                self->storage.result.emplace(detail::invoke_user_body(self->body(), *self->inst));
            c->result_ptr = &*self->storage.result;
        }
    }
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

template<typename T, typename Body>
void Access_op<T, Body>::State::settle_thunk(detail::Task_control_block* c)
{
    // A cancelled nested child is ordering-only (the flag is read before the body, which
    // already ran) - the op completes normally.
    static_cast<State*>(c)->finish(false);
}

template<typename T, typename Body>
void Access_op<T, Body>::State::finish(bool cancel)
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
template<typename T, typename Body>
void Access_op<T, Body>::State::op_settle(bool cancel)
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

template<typename T, typename Body>
Access_op<T, Body>::Access_op(detail::Pipe& pipe, Inst* inst, Body body, Access_options opts, Named name)
    : state_(std::move(body), std::move(opts.token), opts.priority, opts.queued)
{
    state_.pipe = &pipe;
    state_.inst = inst;
    state_.epoch = detail::pipe_epoch(pipe);
    state_.rank = detail::pipe_rank(pipe);
    {
        detail::Task_ptr self(&state_, detail::Adopt_ref{});   // borrowed wrapper, defused below
        detail::set_task_name(self, name);
        self.release();
    }
    state_.started = true;
    fire();
}

template<typename T, typename Body>
void Access_op<T, Body>::fire()
{
    detail::Pipe& pipe = *state_.pipe;
    settled_sync_ = false;
    // Borrowed wrapper for this frame's machinery calls - defused before every return
    // (`this` outlives the call by the single-owner contract; the refcount stays untouched).
    detail::Task_ptr self(&state_, detail::Adopt_ref{});
    detail::bind_pipe_link(&state_, 0, pipe, mode);
    // Reentrant arm (waiting rule (b)): the calling task already holds this pipe's write
    // grant - run inline under it, touching the pipe not at all (the link above is
    // diagnostics only; `run` overwrites the unused turn lock).
    detail::Task_control_block* owner = pipe.writer_owner.load(std::memory_order_acquire);
    if (owner != nullptr && owner == detail::Current_task::get())
    {
        state_.settle_synchronously = true;
        state_.execute(self);
        // `run` cleared the flag if the body attached nested children - the op is then
        // still in flight when this call returns, and the destructor must synchronize.
        settled_sync_ = state_.settle_synchronously;
        self.release();
        return;
    }
    state_.num_locks.store(1, std::memory_order_relaxed);   // the one pipe turn
    // Inline fast path: claim an idle pipe and run on this thread; else enqueue - the
    // admitted turn's release dispatches the body (borrowed route, no machinery refs).
    // `.queued` skips only this arm (never-inline is a dispatch preference); the reentrant
    // arm above is correctness, not opportunism, so it ran regardless.
    if (!state_.queued)
    {
        state_.settle_synchronously = true;
        if (detail::pipe_try_inline(global_scheduler(), pipe, mode, self))
        {
            settled_sync_ = state_.settle_synchronously;   // false if the body attached children
            self.release();
            return;
        }
        state_.settle_synchronously = false;
    }
    self.release();
    detail::pipe_enter_first(&state_, nullptr);
}

template<typename T, typename Body>
void Access_op<T, Body>::start()
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

template<typename T, typename Body>
Access_op<T, Body>::~Access_op()
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

template<typename T, typename Body>
bool Access_op<T, Body>::is_done() const noexcept
{
    return state_.ready.load(std::memory_order_acquire);
}

template<typename T, typename Body>
bool Access_op<T, Body>::is_cancelled() const noexcept
{
    return state_.ready.load(std::memory_order_acquire) && state_.cancelled;
}

template<typename T, typename Body>
void Access_op<T, Body>::wait_settled()
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

template<typename T, typename Body>
detail::Sync_ref_t<typename Access_op<T, Body>::result_type> Access_op<T, Body>::sync() &
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

template<typename T, typename Body>
typename Access_op<T, Body>::result_type Access_op<T, Body>::sync() &&
{
    if constexpr (std::is_void_v<result_type>)
        wait_settled();
    else
        return take();   // the temporary form is a consume; same contract, same fatals
}

template<typename T, typename Body>
typename Access_op<T, Body>::result_type Access_op<T, Body>::take()
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

template<typename T, typename Body>
std::optional<typename Access_op<T, Body>::result_type> Access_op<T, Body>::try_take()
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

namespace detail
{
template<typename T, typename Body>
Task_control_block* access_op_core(Access_op<T, Body>& op) noexcept
{
    return &op.state_;
}

template<typename T, typename Body>
bool* access_op_consumed(Access_op<T, Body>& op) noexcept
{
    return &op.state_.consumed;
}

template<typename T, typename Body>
bool access_op_started(const Access_op<T, Body>& op) noexcept
{
    return op.state_.started;
}
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
        return Access_op<T, std::decay_t<Fn>>(pipe_, &instance_, std::forward<Fn>(fn), opts,
                                              detail::named_from(opts, site));
    }

    // access, read_only.
    template<typename Fn>
        requires detail::Read_only_accessor<Fn, T>
    [[nodiscard("the attended verb: consume the op (co_await, .sync(), try_take()). To not wait, use async")]]
    auto access(Fn&& fn, Access_options opts = {},
                std::source_location site = std::source_location::current()) const
        -> Access_op<T, std::decay_t<Fn>>
    {
        return Access_op<T, std::decay_t<Fn>>(pipe_, &instance_, std::forward<Fn>(fn), opts,
                                              detail::named_from(opts, site));
    }

    // async, read_write: always enqueued (never inline).
    template<typename Fn>
        requires detail::Read_write_accessor<Fn, T>
    auto async(Fn&& fn, Access_options opts = {},
               std::source_location site = std::source_location::current())
        -> Task<detail::Accessor_result_t<Fn, T, Access::read_write>>
    {
        return launch<detail::Accessor_result_t<Fn, T, Access::read_write>, Access::read_write>(
            &instance_, std::forward<Fn>(fn), opts, detail::named_from(opts, site));
    }

    // async, read_only: always enqueued (never inline).
    template<typename Fn>
        requires detail::Read_only_accessor<Fn, T>
    auto async(Fn&& fn, Access_options opts = {},
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
    Task<R> launch(Inst* inst, Fn&& fn, Access_options opts, Named name,
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
    static Task<void> commit_write(Guarded<T>& t, Fn&& fn, Access_options opts, Named name, Task_ptr* record)
    {
        return t.template launch<void, Access::read_write>(
            &t.instance_, std::forward<Fn>(fn), opts, name, record);
    }
};

} // namespace detail

// Defined here rather than with the other `Access_op` members: both need `Guarded`'s
// internals (`Guarded_access`), which are only complete below the class.
template<typename T, typename Body>
Access_op<T, Body>::Access_op(Dormant, Guarded<T>& target, Body body, Access_options opts,
                              std::source_location site)
    : state_(std::move(body), std::move(opts.token), opts.priority, opts.queued)
{
    detail::Pipe& pipe = detail::Guarded_access::pipe(target);
    state_.pipe = &pipe;
    state_.inst = detail::Guarded_access::instance(target);
    state_.epoch = detail::pipe_epoch(pipe);
    state_.rank = detail::pipe_rank(pipe);
    {
        detail::Task_ptr self(&state_, detail::Adopt_ref{});
        detail::set_task_name(self, detail::named_from(opts, site));
        self.release();
    }
}

template<typename T, typename Body>
void Access_op<T, Body>::bind(Guarded<T>& target, Body body)
{
#if TS_SAFETY_CHECKS
    if (state_.started && !state_.ready.load(std::memory_order_acquire))
        ts::fatal("Access_op::bind() while the access is in flight - the queued entry still "
                  "references the current target");
#endif
    if (state_.bound)
        std::destroy_at(&state_.body());
    ::new (static_cast<void*>(state_.body_store)) Body(std::move(body));
    detail::Pipe& pipe = detail::Guarded_access::pipe(target);
    state_.pipe = &pipe;
    state_.inst = detail::Guarded_access::instance(target);
    state_.epoch = detail::pipe_epoch(pipe);
    state_.rank = detail::pipe_rank(pipe);
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
