#pragma once

// Coroutine support - makes `ts::Task<R>` awaitable and provides a `promise_type`
// so a coroutine can return `Task<R>` and `co_await` other tasks. Header-only;
// coroutines are mandatory (docs/coroutine-first.md), so the library assumes a
// coroutine-capable toolchain. Tasks are eager (`initial_suspend = suspend_never`): a body
// runs on the caller's thread until its first real suspension, and awaiting an
// already-settled task never suspends. A coroutine task costs one allocation (the frame),
// a resumed body keeps its creator's access grants on whatever worker it wakes on, and the
// task completes only when any nested work the body attached has settled.
//
// Contents: the `co_await` machinery for `Task<R>` (`Task_awaiter`, the `operator co_await`s,
// `as_optional`), the fused promise (`Promise_base`/`Task_promise`), the held-grant guards
// (`ts::read_write`/`read_only` -> `Access_guard`/`Multi_access_guard` and their awaiters),
// and the bounded resume trampoline. Design notes live with each piece below.

#include "ts/task.h"
#include "ts/guarded.h"   // Pipe, pipe_acquire/release, Guarded(_access), global_scheduler
#include "ts/detail/suspension_registry.h"   // tier 3 of the deadlock report

#include <atomic>
#include <concepts>
#include <coroutine>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace ts
{
namespace detail
{

// The non-template part of a coroutine promise: everything a type-erased
// `Task_control_block*` has to be able to reach. `Promise_base<Derived>` is a template, so
// nothing can downcast to it without naming `Derived`; this base can be recovered from any
// block whose `flags.coroutine` is set.
struct Coroutine_block : Task_control_block
{
    // A frame is never submitted (eager start, trampoline resume), so the block's priority is
    // never a dispatch key for the frame itself; it is what sub-work launched from the body
    // inherits (`parallel_for` helpers), so it is inherited in turn from the task that
    // created the frame - `normal` when created outside a task.
    Coroutine_block() noexcept
    {
        flags.coroutine = true;
        if (const Task_control_block* creator = Current_task::get())
            flags.priority = creator->flags.priority;
    }

#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
    // How many `Access_guard`s this task currently holds (the async-lock guards below).
    // Any `co_await` issued while the count is nonzero is the lock-across-suspension
    // anti-pattern and faults - the harness doubling as a suspension detector.
    // Adjusted by `Access_guard`/`Multi_access_guard`, read by `check_await_under_guard`.
    //
    // A guard exists only inside a coroutine frame (`co_await ts::read_write(obj)` is the
    // only way to mint one) and "is a guard live" is a property of the TASK, not of the
    // thread running it, so the count lives here rather than in thread-local storage. The
    // per-thread spelling was only ever correct because the rule forbids a guard spanning a
    // suspension, which made the invariant argue for itself - and it needed a no-inline
    // barrier to stop a resumed segment reading the suspending thread's slot.
    //
    // One byte: nesting past 8 already exceeds `Access_context::max_entries`, and the byte
    // lands in padding the promise had anyway.
    std::uint8_t guard_depth = 0;
#endif
};

// The running coroutine's promise, or null when the running task is not a coroutine (a
// functor `async` body, a graph node, a bare `launch`) or nothing is running at all.
inline Coroutine_block* current_coroutine_block() noexcept
{
    Task_control_block* blk = Current_task::get();
    return blk != nullptr && blk->flags.coroutine ? static_cast<Coroutine_block*>(blk) : nullptr;
}

// Guards live in this task. Zero outside a coroutine, where none can exist, and zero
// throughout when the rule is compiled out and nothing counts them.
inline int current_guard_depth() noexcept
{
#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
    const Coroutine_block* blk = current_coroutine_block();
    return blk != nullptr ? blk->guard_depth : 0;
#else
    return 0;
#endif
}

#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
// Enter/leave a guard's hold. Called from the guard's constructor and destructor, which run
// in the body of the coroutine that awaited the guard into existence - so the promise is the
// running task both times. A guard outside a coroutine has no owner to charge and no rule to
// enforce, which is a machinery bug, not a user error.
inline void guard_depth_add(int delta) noexcept
{
    Coroutine_block* blk = current_coroutine_block();
    if (blk == nullptr)
        ts::fatal("an access guard was created or destroyed outside the coroutine that owns it");
    blk->guard_depth = static_cast<std::uint8_t>(blk->guard_depth + delta);
}
#endif

// The guard-across-suspension check. It lives at `co_await` entry (`await_ready`), not at
// the suspension (TODO 6.11): a `co_await` that happens to complete synchronously is just
// as illegal as one that suspends, and gating on "did it actually suspend" made the check
// fire only on the runs where timing was unfriendly - so a hold-then-await shipped
// undiagnosed whenever the target pipe was momentarily free.
//
// `Rule::await_under_guard` (ts/rules.h) is a structural rule: it has no scoped opt-out,
// because a guard that did survive a suspension would leave the resumed segment installing
// the promise's access snapshot over the guard's own context, and the guard's destructor
// restoring a `current_access` pointer captured on another thread. The rule protects an
// implementation invariant, not merely a user-level hazard. It can be compiled out
// wholesale (`TS_ENABLED_RULES`), which also removes the counter.
//
// The reentrancy exemption used to be emergent (a reentrant same-object access never
// suspends, so it never reached the check). Hoisting forces it to be stated: see
// `reentrant_under_held_grant` below. Everything else is illegal, guard depth > 0.
inline void check_await_under_guard(const char* message)
{
#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
    if (current_guard_depth() > 0 && rule_enforced(Rule::await_under_guard))
        ts::fatal(message);
#else
    (void)message;
#endif
}

// The one stated exemption to the rule above: an access that cannot suspend by construction
// rather than by timing, because everything it needed was already held by this task. Two
// spellings of the same condition.
//
// (1) `Access_op` lent every object it declares (`bind_links`, waiting rule (b)): it entered
// no pipe, ran inline under the caller's grants, and was settled before the `co_await` was
// evaluated. The flag is what says so - an all-lent block has no links left to inspect.
//
// (2) Every pipe of a block that did take turns is currently write-owned by this task
// (`writer_owner`, always-on state, so this works in every build).
//
// Deliberately narrow. A read access under a read guard is not exempt: it reaches the pipe,
// and if a writer is queued ahead it enqueues and suspends - with our own read hold
// blocking that writer. That is the genuine hazard, not a false positive.
inline bool reentrant_under_held_grant(const Task_control_block* blk) noexcept
{
    if (blk == nullptr)
        return false;
    if (blk->flags.all_lent)
        return true;
    if (blk->pipe_count == 0)
        return false;
    const Task_control_block* running = Current_task::get();
    for (std::uint8_t i = 0; i < blk->pipe_count; ++i)
    {
        const Pipe* pipe = blk->pipe_links[i].pipe;
        if (pipe == nullptr || pipe->writer_owner.load(std::memory_order_acquire) != running)
            return false;
    }
    return true;
}

#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
// `Rule::access_rank` (TODO 6.14). Batch acquisition is already deadlock-free - a node's
// declared set and a multi-object `ts::access` are taken in canonical pipe-address order,
// all-or-nothing. Nothing orders a grant a task already holds against an object it awaits
// later, and that one missing constraint is the whole suspended-ABBA hole. A declared rank
// closes it: every dynamic await must strictly climb, so a cycle is unrepresentable
// (Havender). Unlike the circular-wait detector this fires deterministically on the first
// offending await, rather than needing both halves of a cycle to be suspended at once.
//
// O(1): one scan of the (<= 8 entry) access context against one field, on the cold await
// path only. Declared in `guarded.cpp` so the message can name the objects.
[[noreturn]] void report_rank_violation(const Pipe* awaited, unsigned held_max,
                                        bool held_unranked) noexcept;

// Check one awaited pipe against the current context. Skips objects the context already
// grants: awaiting something you hold is not a later acquisition (it is reentrancy, or a
// lend), so it cannot extend a wait chain.
inline void check_access_rank(const Pipe* pipe, const void* instance) noexcept
{
    if (pipe == nullptr || access_load() == nullptr || !rule_enforced(Rule::access_rank))
        return;
    if (instance != nullptr && access_load()->check(instance, Access::read_only) != Access_context::Grant::none)
        return;   // already held - not a later acquisition
    const Access_context::Rank_state held = access_load()->rank_state();
    if (held.held == 0)
        return;   // nothing held: a top-level await orders against nothing
    const unsigned target = pipe_rank(*pipe);
    if (held.any_unranked || target == 0 || target <= held.max)
        report_rank_violation(pipe, held.max, held.any_unranked);
}

// The awaited task's pipes, if it is an access. A bare task orders against nothing.
inline void check_access_rank(const Task_control_block* blk) noexcept
{
    if (blk == nullptr)
        return;
    for (std::uint8_t i = 0; i < blk->pipe_count; ++i)
        check_access_rank(blk->pipe_links[i].pipe, nullptr);
}
#else
inline void check_access_rank(const Pipe*, const void*) noexcept {}
inline void check_access_rank(const Task_control_block*) noexcept {}
#endif

inline constexpr const char* await_under_guard_message =
    "co_await while holding a Guarded access guard: the guarded object would be held across the "
    "suspension, and the guard's own access context cannot survive a resume on another thread. "
    "Use the functor form co_await obj.access(fn), or split the scope (release the guard, "
    "await, re-acquire). This rule is structural - it has no runtime opt-out; a build can drop "
    "it wholesale with TS_ENABLED_RULES";

// Segment bracket, resolved per promise type: enter installs the coroutine's `Current_task`
// (and, in the promise, its access snapshot is applied by the resume path); exit restores.
// A foreign promise (some other library's coroutine awaiting our task) has neither - both
// no-op for it.
template<typename P>
inline void enter_segment_if_ours(P& promise)
{
    if constexpr (requires { promise.enter_segment(); })
        promise.enter_segment();
}

template<typename P>
inline void exit_segment_if_ours(P& promise)
{
    if constexpr (requires { promise.exit_segment(); })
        promise.exit_segment();
}

// Resume a coroutine, re-installing its captured access grant and its `Current_task`
// segment state. The async resume runs on the thread that settled the awaited task /
// granted the pipe - whose ambient state is not the coroutine's. The scope's destructor
// only touches its saved `prev_`, so it stays valid even though `h.resume()` may destroy
// the frame when the coroutine completes (the segment state is restored by the awaiter /
// final awaiter before any destruction).
template<typename P>
inline void resume_with_access(std::coroutine_handle<P> h)
{
    if constexpr (requires { h.promise().access_ctx_; })
    {
        enter_segment_if_ours(h.promise());
        Inherited_access_scope scope(h.promise().access_ctx_);
        h.resume();
    }
    else
    {
        h.resume();
    }
}

// Bounded coroutine-resume trampoline. Without it a cascade of coroutine completions recurses
// and overflows the stack for a deep chain: coroutine A's completion resumes B (which awaited
// A), whose completion resumes C, ... This mirrors `Task_control_block::Inline_queue`
// (task.h) in spirit - we can't reuse that vector because it is typed to `Task_ptr` and
// drives `block->execute()`, whereas here we drive a `coroutine_handle`. The first resume on a
// thread starts a drain; a resume requested during the drain (the cascade) is pushed and run by
// the outer loop, so the whole chain runs iteratively (O(1) stack). Type-erased to a thunk + one
// pointer -> no per-resume allocation (the vector retains capacity across drains, like task.h's).
// The resume still runs on the settling thread (no queue hop -> lowest latency), just un-nested.
struct Resume_item
{
    void (*thunk)(void*);
    void* handle;
};

// The queue itself, behind the thread-local barrier (ts/detail/thread_local.h). A vector
// cannot be passed by value, so the barrier is the whole operation rather than an accessor
// pair: nothing outside can name the queue, and the one function that touches it is out of
// line, so its address is resolved on the thread that pushes. That the drain then stays on
// one thread is a property of the drain, not of the caller - `h.resume()` returns to this
// stack whether the resumed frame suspends again or completes, so the loop's later reads see
// the slot it started with.
class Resume_queue
{
public:
    // Queue a resume and, unless a drain is already running on this thread, run the whole
    // chain iteratively.
    TS_DETAIL_NO_INLINE static void push_and_drain(Resume_item item);

private:
    inline static thread_local std::vector<Resume_item> pending_;
    inline static thread_local bool draining_ = false;
};

inline void Resume_queue::push_and_drain(Resume_item item)
{
    pending_.push_back(item);
    if (draining_)
        return;   // an active drain on this thread will pick it up - don't recurse
    draining_ = true;
    for (std::size_t head = 0; head < pending_.size(); ++head)
    {
        Resume_item next = pending_[head];   // copy: a nested push may realloc the vector
        next.thunk(next.handle);             // re-install ambient state + h.resume()
    }
    pending_.clear();   // retains capacity -> no steady-state allocation
    draining_ = false;
}

template<typename P>
void resume_thunk(void* addr)
{
    resume_with_access(std::coroutine_handle<P>::from_address(addr));
}

template<typename P>
void schedule_resume(std::coroutine_handle<P> h)
{
    Resume_queue::push_and_drain({ &resume_thunk<P>, h.address() });
}

// Awaiter for `co_await task`. Holds an owning `core_` (keeps the awaited block alive across
// the suspension) plus a two-state handshake that resolves the race between `await_suspend`
// finishing its suspend and the completion callback firing.
template<typename R>
struct Task_awaiter
{
    explicit Task_awaiter(Task_ptr core) noexcept
        : core_(std::move(core))
    {}

    Task_awaiter(const Task_awaiter&) = delete;
    Task_awaiter& operator=(const Task_awaiter&) = delete;

    bool await_ready() const noexcept
    {
        if (!reentrant_under_held_grant(core_.get()))
        {
            check_await_under_guard(await_under_guard_message);
            check_access_rank(core_.get());
        }
        return core_->ready.load(std::memory_order_acquire);
    }

    // Register the resume, resolving the synchronous-fire hazard: `attach` fires the callback
    // immediately if the block settled in the window since `await_ready` (closing the lost
    // wakeup). Were we to `h.resume()` inline there, the coroutine could run to completion and
    // destroy this frame (and this awaiter) while we are still inside `await_suspend` - a
    // use-after-free. The handshake: whoever reaches `state_` first (the callback via
    // exchange(1), or the tail of `await_suspend` via exchange(2)) loses the resume; the
    // second one performs it.
    //
    // Segment ordering: `exit_segment` runs before the handshake publishes the frame for
    // resumption (the release-exchange), so a cross-thread resume's `enter_segment` never
    // overlaps this thread's save/restore of the segment slots; on the lost-exchange path
    // (synchronous fire) the segment is re-entered and the body continues uninterrupted.
    template<typename P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        // The guard-across-suspension rule was already checked at `co_await` entry
        // (`await_ready`), so reaching here means it passed.
#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)
        // Waits-for edges (docs/coroutine-first.md §2): suspending on a pipe job while this
        // context holds grants - the job's turn cannot arrive until its pipes drain, so a
        // held-grant cycle through them is the suspended-ABBA deadlock. Recorded before the
        // attach (the segment's `current_access` is still installed here); cleared at
        // `await_resume`. Non-pipe tasks record nothing.
        if (core_->pipe_count > 0 && access_load() != nullptr && rule_enforced(Rule::circular_wait))
        {
            Pipe* awaited[Access_context::max_entries];   // sized to the object cap, so it never truncates
            int n = 0;
            for (std::uint8_t i = 0; i < core_->pipe_count && n < Access_context::max_entries; ++i)
                awaited[n++] = core_->pipe_links[i].pipe;
            recorded_ = circular_wait_record(access_load(), this, Current_task::get(), awaited, n);
        }
#endif

#if TS_SUSPENSION_REGISTRY
        suspension_.awaited_task = core_->name_or_empty();
        suspension_link(suspension_);
        registered_ = true;
#endif

        exit_segment_if_ours(h.promise());

        core_->attach([this, h](void*, bool)
        {
            if (state_.exchange(1, std::memory_order_acq_rel) == 2)
                schedule_resume(h);   // await_suspend already suspended -> we own the resume
        });

        if (state_.exchange(2, std::memory_order_acq_rel) == 1)
        {
            enter_segment_if_ours(h.promise());
            return false;     // callback already fired synchronously -> resume via await_resume
        }
        return true;          // suspended; the callback will resume when the task settles
    }

    decltype(auto) await_resume()
    {
        end_wait();
        if constexpr (std::is_void_v<R>)
        {
            return;   // a cancelled void task simply resumes (mirrors sync())
        }
        else
        {
            if (core_->cancelled)
                ts::fatal("co_await on a cancelled task has no result; check is_cancelled() first");
            return *static_cast<const R*>(core_->result_ptr);   // const R&, non-consuming
        }
    }

    // Retire any wait edges this suspension recorded. Shared with the `as_optional`
    // awaiter below, which resumes differently but ends the same wait.
    void end_wait() noexcept
    {
#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)
        if (recorded_)
            circular_wait_clear(this);
#endif
#if TS_SUSPENSION_REGISTRY
        if (registered_)
        {
            suspension_unlink(suspension_);
            registered_ = false;
        }
#endif
    }

    Task_ptr core_;
    std::atomic<int> state_{ 0 };
#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)
    bool recorded_ = false;   // wait edges recorded at suspend, cleared at resume
#endif
#if TS_SUSPENSION_REGISTRY
    Suspension_record suspension_;   // in the coroutine frame; never allocated
    bool registered_ = false;
#endif
};

// `co_await t.as_optional()` - the same wait, resolving to `std::optional<R>` instead of
// fatalling when the task settled cancelled. Moves the result out (like `take()`), so it
// must be the last consume.
template<typename R>
struct Optional_awaiter : Task_awaiter<R>
{
    using Task_awaiter<R>::Task_awaiter;

    std::optional<R> await_resume()
    {
        this->end_wait();
        if (this->core_->cancelled)
            return std::nullopt;
#if TS_SAFETY_CHECKS
        // Consumes like `take()`, so it claims the block's consume flag - a `take()` after
        // this one is the checked fatal, not a silent moved-from result.
        if (this->core_->result_consumed.exchange(true, std::memory_order_acq_rel))
            return std::nullopt;
#endif
        return std::move(*static_cast<R*>(this->core_->result_ptr));
    }
};

// In `ts::detail`, not `ts`: the argument is `detail::Optional_awaitable`, so that is where
// ADL looks for the operator.
template<typename R>
Optional_awaiter<R> operator co_await(Optional_awaitable<R> awaitable)
{
    return Optional_awaiter<R>(std::move(awaitable.core));
}

// The shared (result-agnostic) half of the fused promise. The block is a base subobject
// (the `Executable` pattern): a `Task_control_block*` recovers the promise with a
// `static_cast`, and the block's `destroy` destroys the whole coroutine frame. Derives
// from `Coroutine_block` (itself a `Task_control_block`) rather than `Block_backed` - the
// frame, not the promise, is what `destroy` must tear down. `num_locks` is armed to `execution_flag + 1`
// (executing + the body self-lock) in the constructor, so `detail::add_nested` can attach a
// gating child (a nested graph run) to this coroutine across all its segments; the final
// awaiter drops the self-lock - the task completes at `co_return` when no children are
// pending, else when the last child settles.
template<typename Derived>
struct Promise_base : Coroutine_block
{
    // The ambient access grant at creation (empty if created outside any task), re-installed
    // around each resumed segment.
    std::optional<Access_context> access_ctx_ = snapshot_access();
    // Saved `Current_task` of the enclosing segment; valid only while this coroutine's
    // segment is installed. Written/read only by the thread running the segment (the
    // suspension handshake orders cross-thread handoffs).
    Task_ptr prev_task_;
    // The frame's implicit scope: a nested graph run started in any segment is recorded here
    // (in addition to the completion lock it takes) so the graph's non-quiet-scope lending
    // check (static_task_graph.cpp) can see a still-running run. Same single-thread-per-segment
    // discipline as `prev_task_`.
    std::vector<Task_ptr> scope_children_;
    std::vector<Task_ptr>* prev_scope_ = nullptr;
    // The frame's rule relaxation (`ts::Relaxed_scope`), carried across suspensions the same
    // way: a `Relaxed_scope` entered in the body must still be in effect when the body resumes
    // on another worker, and must not leak onto that worker (docs/waiting-rule-policy.md §4).
    Relaxed_carrier relaxed_;

    Promise_base()
    {
        destroy = &destroy_frame;
        // The "running" self-reference: keeps the frame alive while the body runs even if
        // every external handle is dropped (fire-and-forget). Released by the final awaiter.
        refcount.store(1, std::memory_order_relaxed);
        // Executing + body self-lock: nested children add completion locks (task.h §4 regime).
        num_locks.store(Task_control_block::execution_flag + 1, std::memory_order_relaxed);
        inherit_task_name(*this);   // before `enter_segment` installs this frame as current
        enter_segment();   // the eager body runs on the caller right after the promise ctor
    }

    static void destroy_frame(Task_control_block* c)
    {
        auto& promise = *static_cast<Derived*>(c);
        std::coroutine_handle<Derived>::from_promise(promise).destroy();
    }

    // The segment bracket. Every piece of ambient state it swaps sits behind the thread-local
    // barrier (ts/detail/thread_local.h): this runs inlined into the frame on both sides of
    // every suspension, so an address resolved here and reused after the resume would be the
    // suspending thread's.
    void enter_segment()
    {
        prev_task_ = Current_task::exchange(Task_ptr(this));
        prev_scope_ = Scope_children::exchange(&scope_children_);
        relaxed_.enter();
    }

    void exit_segment()
    {
        relaxed_.exit();
        Current_task::exchange(std::move(prev_task_));
        Scope_children::store(prev_scope_);
    }

    std::suspend_never initial_suspend() const noexcept { return {}; }

    // Final awaiter: restore the segment, drop the body self-lock (completing the task if no
    // nested children are pending - otherwise the last child completes it), then release the
    // running self-reference. The frame is destroyed here iff nothing else holds a ref; with
    // children or handles outstanding it lives until the last of them drops (so the result and
    // the coroutine's parameters stay valid for them).
    struct Final_awaiter
    {
        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<Derived> h) noexcept
        {
            auto& promise = h.promise();
            promise.exit_segment();
            Task_control_block* c = &promise;
            if (c->num_locks.fetch_sub(1, std::memory_order_acq_rel) == Task_control_block::execution_flag + 1)
                c->complete();
            intrusive_dec(c);   // may destroy the frame; touch nothing afterwards
        }

        void await_resume() const noexcept {}   // never resumed
    };

    Final_awaiter final_suspend() noexcept { return {}; }

    // The coroutine arm of the body boundary the functor seams enforce
    // (`detail::invoke_user_body`), reported the same way. A promise must declare this
    // whatever the build's exception support is; with none, nothing can reach it.
    void unhandled_exception()
    {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        try
        {
            throw;   // re-raise, to read the exception's own text where there is one
        }
        catch (const std::exception& e)
        {
            detail::escaped_exception_diagnose(e.what());
        }
        catch (...)
        {
            detail::escaped_exception_diagnose(nullptr);
        }
#else
        detail::escaped_exception_diagnose(nullptr);
#endif
    }
};

// Promise for a coroutine returning `Task<R>` - the fused frame+block (see header comment).
template<typename R>
struct Task_promise : Promise_base<Task_promise<R>>
{
    Result_storage<R> storage;

    Task<R> get_return_object() { return Task<R>(Task_ptr(this)); }

    void return_value(R value)
    {
        storage.result.emplace(std::move(value));
        this->result_ptr = &*storage.result;
        // Completion happens in the final awaiter (after the self-lock drop), uniform with
        // the nested-children gate.
    }
};

template<>
struct Task_promise<void> : Promise_base<Task_promise<void>>
{
    Task<void> get_return_object() { return Task<void>(Task_ptr(this)); }

    void return_void() {}   // completion happens in the final awaiter
};

// The access-guard awaiters, defined below - named here so each guard can friend its creator.
template<typename T, Access Mode> struct Access_awaiter;
template<Access Mode, typename... Ts> struct Multi_access_awaiter;

} // namespace detail

// RAII async-lock guard over a `Guarded<T>`, held for direct `T` access. Returned by
// `co_await ts::read_only(w)` / `ts::read_write(w)`. non-copyable and non-movable on purpose: it installs
// `current_access = &ctx_` (a member), so its address must be stable - `await_resume` returns it
// as a prvalue and `auto g = co_await ...;` constructs it in place via guaranteed copy elision
// (a move would dangle the installed pointer; non-movable makes a stray copy a compile error).
// While the guard is alive `current_access` grants `obj_` in `Mode`, so `g->method()` passes the
// harness; the access is held (readers concurrent, writer exclusive) until the guard is destroyed.
template<typename T, Access Mode>
class Access_guard
{
public:
    ~Access_guard()
    {
        detail::access_store(prev_);
#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
        detail::guard_depth_add(-1);
#endif
        detail::pipe_release(scheduler_, pipe_, Mode);   // admit queued entries / the next guard
    }

    Access_guard(const Access_guard&) = delete;
    Access_guard& operator=(const Access_guard&) = delete;

    decltype(auto) operator*() const
    {
        if constexpr (Mode == Access::read_write)
            return static_cast<T&>(*obj_);
        else
            return static_cast<const T&>(*obj_);
    }

    auto operator->() const
    {
        if constexpr (Mode == Access::read_write)
            return obj_;
        else
            return static_cast<const T*>(obj_);
    }

private:
    // The grant is minted by awaiting `ts::read_only`/`ts::read_write`, never constructed by
    // hand: the ctor takes the already-acquired pipe and would otherwise let a caller forge a
    // grant (and release a pipe it never took). Keeping it private is also what keeps
    // `detail::Pipe` out of every public signature.
    friend struct detail::Access_awaiter<T, Mode>;

    Access_guard(Scheduler& scheduler, detail::Pipe& pipe, T* obj)
        : scheduler_(scheduler)
        , pipe_(pipe)
        , obj_(obj)
    {
        if (const Access_context* cur = detail::access_load())
            ctx_ = *cur;   // extend the coroutine's existing grant, don't replace it
        ctx_.add(obj_, Mode, detail::pipe_epoch(pipe_), detail::pipe_rank(pipe_));
        prev_ = detail::access_load();
        detail::access_store(&ctx_);
#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
        detail::guard_depth_add(1);
#endif
    }

    Scheduler& scheduler_;
    detail::Pipe& pipe_;
    T* obj_;
    Access_context ctx_;
    const Access_context* prev_ = nullptr;
};

// RAII scope guard holding grants on SEVERAL `Guarded`s at once, returned by the multi-object
// `co_await ts::read_write(a, b, ...)` / `ts::read_only(a, b, ...)`. Like the single-object
// `Access_guard` it is non-copyable and non-movable (its `ctx_` is the installed
// `current_access`), so it is direct-initialized in place by guaranteed copy elision. The
// guarded objects are reached by index - `guard.get<I>()` - or, more usually, by structured
// bindings: `auto [a, b] = co_await ts::read_write(x, y);` via the tuple protocol below. All
// objects share one `Mode` in v1; mixed per-object modes are a follow-up (until then use the
// callback `ts::access(fn, as_read_only(a), as_read_write(b))` when modes must differ). The
// awaiter acquires the pipes in canonical (pipe-address) order, deadlock-free; the guard
// releases each on scope exit.
template<Access Mode, typename... Ts>
class Multi_access_guard
{
public:
    static constexpr std::size_t arity = sizeof...(Ts);

    ~Multi_access_guard()
    {
        detail::access_store(prev_);
#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
        detail::guard_depth_add(-1);
#endif
        for (std::size_t i = 0; i < pipe_count_; ++i)
            detail::pipe_release(scheduler_, *pipes_[i], Mode);
    }

    Multi_access_guard(const Multi_access_guard&) = delete;
    Multi_access_guard& operator=(const Multi_access_guard&) = delete;

    // The I-th object, in declaration order: `T_I&` for write, `const T_I&` for read. Backs
    // both `guard.get<I>()` and structured bindings (member `get` is found first).
    template<std::size_t I>
    decltype(auto) get() const
    {
        using T = std::tuple_element_t<I, std::tuple<Ts...>>;
        auto* obj = detail::Guarded_access::instance(*std::get<I>(objs_));
        if constexpr (Mode == Access::read_write)
            return static_cast<T&>(*obj);
        else
            return static_cast<const T&>(*obj);
    }

private:
    // Minted by awaiting the multi-object `ts::read_only`/`ts::read_write` only - see
    // `Access_guard`'s note; the ctor adopts pipes the awaiter already acquired.
    friend struct detail::Multi_access_awaiter<Mode, Ts...>;

    Multi_access_guard(Scheduler& scheduler, std::tuple<Guarded<Ts>*...> objs,
                       detail::Pipe* const* pipes, std::size_t pipe_count)
        : scheduler_(scheduler)
        , objs_(objs)
        , pipe_count_(pipe_count)
    {
        for (std::size_t i = 0; i < pipe_count_; ++i)
            pipes_[i] = pipes[i];
        if (const Access_context* cur = detail::access_load())
            ctx_ = *cur;   // extend the coroutine's existing grant
        add_all(std::index_sequence_for<Ts...>{});
        prev_ = detail::access_load();
        detail::access_store(&ctx_);
#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
        detail::guard_depth_add(1);
#endif
    }

    template<std::size_t... I>
    void add_all(std::index_sequence<I...>)
    {
        (ctx_.add(static_cast<const void*>(detail::Guarded_access::instance(*std::get<I>(objs_))), Mode,
                  detail::pipe_epoch(detail::Guarded_access::pipe(*std::get<I>(objs_))),
                  detail::pipe_rank(detail::Guarded_access::pipe(*std::get<I>(objs_)))),
         ...);
    }

    Scheduler& scheduler_;
    std::tuple<Guarded<Ts>*...> objs_;
    detail::Pipe* pipes_[sizeof...(Ts)];
    std::size_t pipe_count_;
    Access_context ctx_;
    const Access_context* prev_ = nullptr;
};

namespace detail
{

// Awaiter for `co_await ts::read_only(w)` / `ts::read_write(w)`. Acquires the access in `Mode` (holding it),
// then resumes with an `Access_guard`. Exposed publicly as `ts::Access_awaiter` (the alias below), so
// `read_write`/`read_only` name no `detail` type; the definition stays here with the `detail` internals it
// drives (`pipe_acquire`, the resume trampoline, the waits-for detector). The acquire/resume race (a
// deferred acquire's `on_acquired` firing on another thread vs `await_suspend` finishing) uses the same
// two-state handshake as `Task_awaiter`, with the same segment ordering.
template<typename T, Access Mode>
struct Access_awaiter
{
    Access_awaiter(Scheduler& scheduler, Pipe& pipe, T* obj) noexcept
        : scheduler_(scheduler)
        , pipe_(pipe)
        , obj_(obj)
    {}

    Access_awaiter(const Access_awaiter&) = delete;
    Access_awaiter& operator=(const Access_awaiter&) = delete;

    // Must attempt the acquire (side effects), so this never reports ready - but it is
    // still where the guard rule is checked, at `co_await` entry. Acquiring a second guard
    // while one is live is illegal whether or not the pipe happens to be free right now:
    // if it is not, the frame suspends holding the first guard, which is the ABBA shape.
    // No exemption - re-acquiring the same object would queue behind the caller's own hold.
    bool await_ready() const noexcept
    {
        check_await_under_guard(
            "co_await a Guarded access guard while another is live: nested guards hold both "
            "objects across any suspension. Take them together with the multi-object form "
            "co_await ts::access(fn, a, b) (one canonically-ordered acquisition), or release "
            "the first guard before acquiring the second");
        check_access_rank(&pipe_, obj_);
        return false;
    }

    template<typename P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        exit_segment_if_ours(h.promise());

        // Try to acquire now; `pipe_acquire` returns true (held, no callback) or false (deferred,
        // `on_acquired` fires once when the pipe drains to us, possibly on another thread).
        auto on_acquired = [this, h]
        {
            if (state_.exchange(1, std::memory_order_acq_rel) == 2)
                schedule_resume(h);   // re-install ambient state + resume
        };
        // A write hold publishes the awaiting coroutine's task as the grant holder (null on
        // a non-task thread) - so `Deferred::commit` under a coroutine write guard can take
        // its held-grant fast path. `Current_task` was restored by `exit_segment` above, so
        // name the coroutine's own core explicitly.
        Task_control_block* owner = owner_of(h);
        bool acquired = pipe_acquire(scheduler_, pipe_, Mode, std::move(on_acquired), owner);
        if (acquired)
        {
            enter_segment_if_ours(h.promise());
            return false;   // held now -> don't suspend; `await_resume` builds the guard
        }

        // Deferred: we are about to suspend. The guard rule was already checked at
        // `co_await` entry (`await_ready`).
#if TS_SUSPENSION_REGISTRY
        suspension_.awaited_pipe = &pipe_;
        suspension_link(suspension_);
        registered_ = true;
#endif
#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)
        // Waits-for edge (docs/coroutine-first.md §2): a deferred acquire is a genuine
        // suspension on this pipe; record {each held grant -> pipe_} and cycle-check.
        // `current_access` is still the segment's context here (`exit_segment` restores
        // task identity, not the access scope). Cleared at `await_resume`.
        Pipe* awaited = &pipe_;
        if (rule_enforced(Rule::circular_wait))
            recorded_ = circular_wait_record(access_load(), this, owner_of(h), &awaited, 1);
#endif

        if (state_.exchange(2, std::memory_order_acq_rel) == 1)
        {
            enter_segment_if_ours(h.promise());
            return false;   // on_acquired already fired -> resume via await_resume
        }
        return true;        // suspended; on_acquired will resume when the pipe grants
    }

    Access_guard<T, Mode> await_resume() noexcept
    {
#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)
        if (recorded_)
            circular_wait_clear(this);
#endif
#if TS_SUSPENSION_REGISTRY
        if (registered_)
        {
            suspension_unlink(suspension_);
            registered_ = false;
        }
#endif
        return Access_guard<T, Mode>(scheduler_, pipe_, obj_);   // prvalue -> elided into the local
    }

private:
    // Our promise carries the block as a base subobject; a foreign promise has no block,
    // so the owner falls back to the ambient task.
    template<typename P>
    static Task_control_block* owner_of(std::coroutine_handle<P> h) noexcept
    {
        if constexpr (std::derived_from<P, Task_control_block>)
            return static_cast<Task_control_block*>(&h.promise());
        else
            return Current_task::get();
    }

public:
    Scheduler& scheduler_;
    Pipe& pipe_;
    T* obj_;
    std::atomic<int> state_{ 0 };
#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)
    bool recorded_ = false;   // wait edge recorded at suspend, cleared at resume
#endif
#if TS_SUSPENSION_REGISTRY
    Suspension_record suspension_;
    bool registered_ = false;
#endif
};

// Awaiter for the multi-object `co_await ts::read_write(a, b, ...)` (exposed as
// `ts::Multi_access_awaiter` via the alias below). Acquires every object's pipe in canonical
// (pipe-address) order - the same order a node's declared set and a multi-object `ts::access`
// take, so it is deadlock-free against them and against another multi-object guard - holding
// each until all are held, then resumes with a `Multi_access_guard`. Unlike `ts::access` it
// dispatches no body: the final grant resumes the coroutine with the grants held. The acquire
// chain is sequential - each `pipe_acquire` grants inline (advance) or defers (its callback
// continues the chain on the granting worker); the final-grant vs `await_suspend` race uses the
// same two-state handshake as the single-object `Access_awaiter`.
template<Access Mode, typename... Ts>
struct Multi_access_awaiter
{
    static constexpr std::size_t N = sizeof...(Ts);

    explicit Multi_access_awaiter(Scheduler& scheduler, Guarded<Ts>&... objs) noexcept
        : scheduler_(scheduler)
        , objs_(&objs...)
    {
        Pipe* raw[] = { &Guarded_access::pipe(objs)... };
        // Insertion-sort by pipe address, the canonical order `async_build_modes` uses. A
        // repeated object is fatal (same strictness as the multi-object verbs and `add_node`:
        // declare each object once).
        for (std::size_t k = 0; k < N; ++k)
        {
            Pipe* pk = raw[k];
            std::size_t i = 0;
            while (i < pipe_count_ && pipes_[i] < pk)
                ++i;
            if (i < pipe_count_ && pipes_[i] == pk)
                ts::fatal("ts::read_write/ts::read_only: the same Guarded object was passed "
                          "twice - list each object once");
            for (std::size_t j = pipe_count_; j > i; --j)
                pipes_[j] = pipes_[j - 1];
            pipes_[i] = pk;
            ++pipe_count_;
        }
    }

    Multi_access_awaiter(const Multi_access_awaiter&) = delete;
    Multi_access_awaiter& operator=(const Multi_access_awaiter&) = delete;

    // Never ready (the acquire has side effects), but the guard + rank rules are checked here,
    // at `co_await` entry, exactly as the single-object awaiter does.
    bool await_ready() noexcept
    {
        check_await_under_guard(
            "co_await a multi-object access guard while another guard is live: nested guards hold "
            "every object across any suspension. Take all objects in one co_await "
            "ts::read_write(a, b, ...), or release the live guard before acquiring the next");
        for (std::size_t i = 0; i < pipe_count_; ++i)
            check_access_rank(pipes_[i], nullptr);
        return false;
    }

    template<typename P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        exit_segment_if_ours(h.promise());
        owner_ = owner_of(h);
#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)
        held_ctx_ = access_load();   // the coroutine's grants, captured for the wait record
#endif
        drive(0, h, true);
        if (state_.exchange(2, std::memory_order_acq_rel) == 1)
        {
            enter_segment_if_ours(h.promise());
            return false;   // the whole set was acquired inline
        }
        return true;
    }

    Multi_access_guard<Mode, Ts...> await_resume() noexcept
    {
#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)
        if (recorded_)
            circular_wait_clear(this);
#endif
#if TS_SUSPENSION_REGISTRY
        if (registered_)
        {
            suspension_unlink(suspension_);
            registered_ = false;
        }
#endif
        return Multi_access_guard<Mode, Ts...>(scheduler_, objs_, pipes_, pipe_count_);
    }

private:
    // Acquire pipes_[k..) in order. `record_on_block` is true only for the `drive` call from
    // `await_suspend`: a deferral there records the waits-for edge + registry entry against the
    // captured `held_ctx_`. Later deferrals run on the granting worker and pass `false` - not
    // because the ambient state is wrong (`held_ctx_` is captured precisely so it is not) but to
    // keep `recorded_`/`registered_` single-writer: a worker-side record would race the awaiter's
    // record-state against `await_resume`. So only the first-blocked pipe's edge is recorded.
    //
    // The consequence is a diagnostic gap, not a correctness one. The multi-guard's own
    // acquisition is deadlock-free by canonical order regardless. A cross-waiter ABBA through a
    // later-blocked pipe - possible when the coroutine also holds an enclosing grant that is not an
    // `Access_guard` (a node's declared set, which `await_under_guard` does not bar) - goes
    // unrecorded by the circular-wait detector, so `deadlock_net` (the quiescence backstop, tier 2)
    // reports it instead: a coarser, window-delayed report rather than the immediate named-cycle
    // one, never a silent hang. Closing the gap would need per-pipe record state and a thread-safe
    // worker-side record; not worth it given the backstop.
    template<typename P>
    void drive(std::size_t k, std::coroutine_handle<P> h, bool record_on_block)
    {
        while (k < pipe_count_)
        {
            bool got = pipe_acquire(scheduler_, *pipes_[k], Mode,
                [this, h, k] { this->drive(k + 1, h, false); }, owner_);
            if (got)
            {
                ++k;
                continue;
            }
            if (record_on_block)
                record_wait(pipes_[k]);
            return;   // deferred; the callback resumes the chain when this pipe grants
        }
        if (state_.exchange(1, std::memory_order_acq_rel) == 2)
            schedule_resume(h);
    }

    void record_wait([[maybe_unused]] Pipe* pipe) noexcept
    {
#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)
        if (rule_enforced(Rule::circular_wait))
        {
            Pipe* awaited = pipe;
            recorded_ = circular_wait_record(held_ctx_, this, owner_, &awaited, 1);
        }
#endif
#if TS_SUSPENSION_REGISTRY
        suspension_.awaited_pipe = pipe;
        suspension_link(suspension_);
        registered_ = true;
#endif
    }

    // Same block-backed-promise probe as the single-object awaiter's `owner_of`.
    template<typename P>
    static Task_control_block* owner_of(std::coroutine_handle<P> h) noexcept
    {
        if constexpr (std::derived_from<P, Task_control_block>)
            return static_cast<Task_control_block*>(&h.promise());
        else
            return Current_task::get();
    }

    Scheduler& scheduler_;
    std::tuple<Guarded<Ts>*...> objs_;
    Pipe* pipes_[sizeof...(Ts)];
    std::size_t pipe_count_ = 0;
    Task_control_block* owner_ = nullptr;
    std::atomic<int> state_{ 0 };
#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)
    const Access_context* held_ctx_ = nullptr;
    bool recorded_ = false;
#endif
#if TS_SUSPENSION_REGISTRY
    Suspension_record suspension_;
    bool registered_ = false;
#endif
};

} // namespace detail

// Public spelling of the access-guard awaiter, so `read_write`/`read_only` return no `detail` type. Users
// never name it - it is `co_await`ed immediately - but the return type in the header reads `ts::`.
template<typename T, Access Mode>
using Access_awaiter = detail::Access_awaiter<T, Mode>;

// Public spelling of the multi-object access-guard awaiter (returned by the multi-arg
// `read_write`/`read_only`); like `Access_awaiter`, users never name it.
template<Access Mode, typename... Ts>
using Multi_access_awaiter = detail::Multi_access_awaiter<Mode, Ts...>;

// Public spelling of the task awaiter (what `co_await task` builds), for symmetry with
// `ts::Access_awaiter`; like it, users never name it.
template<typename R>
using Task_awaiter = detail::Task_awaiter<R>;

// `co_await task` -> suspend until `task` settles, then resume with its result (`const R&`,
// non-consuming; `void` for a void task). ADL finds these in namespace `ts`.
template<typename R>
Task_awaiter<R> operator co_await(const Task<R>& t)
{
    return detail::Task_awaiter<R>(detail::core_of(t));
}

template<typename R>
Task_awaiter<R> operator co_await(Task<R>&& t)
{
    return detail::Task_awaiter<R>(detail::core_of(t));
}

namespace detail
{

// Awaiter for `co_await obj.access(fn)` / a named `Access_op`. The wait is `Task_awaiter`'s
// (same handshake, same rule checks - the reentrant exemption works off the op's bound link);
// only the resume differs: the result moves out BY VALUE, mirroring `Access_op::sync()` - the
// op owns the storage and, in the temporary form, dies at the end of the full-expression.
// The awaiter's `core_` ref on the op block is balanced within the op's lifetime (the awaiter
// is destroyed before the op, both being parts of the same full-expression / frame).
template<typename R>
struct Access_op_awaiter : Task_awaiter<R>
{
    bool* consumed;   // the op's this-cycle consume flag (unused for void R)

    Access_op_awaiter(Task_ptr core, bool* consumed_flag)
        : Task_awaiter<R>(std::move(core))
        , consumed(consumed_flag)
    {}

    R await_resume()
    {
        this->end_wait();
        if constexpr (std::is_void_v<R>)
        {
            return;   // a cancelled void access simply resumes (mirrors sync())
        }
        else
        {
            if (this->core_->cancelled)
                ts::fatal("co_await on a cancelled access has no result; check is_cancelled() first");
#if TS_SAFETY_CHECKS
            if (*consumed)
                ts::fatal("co_await on an Access_op whose result was already consumed this cycle - "
                          "start() refires before the next consume");
#endif
            *consumed = true;
            return std::move(*static_cast<R*>(this->core_->result_ptr));
        }
    }
};

// `co_await op.as_optional()` - `Access_op_awaiter`'s wait, resolving to `std::optional<R>`
// instead of fatalling when the access settled cancelled. Consumes the cycle's result, like
// the bare await.
template<typename R>
struct Optional_access_awaiter : Task_awaiter<R>
{
    bool* consumed;

    Optional_access_awaiter(Task_ptr core, bool* consumed_flag)
        : Task_awaiter<R>(std::move(core))
        , consumed(consumed_flag)
    {}

    std::optional<R> await_resume()
    {
        this->end_wait();
        if (this->core_->cancelled)
            return std::nullopt;
#if TS_SAFETY_CHECKS
        if (*consumed)
            ts::fatal("co_await on an Access_op whose result was already consumed this cycle - "
                      "start() refires before the next consume");
#endif
        *consumed = true;
        return std::move(*static_cast<R*>(this->core_->result_ptr));
    }
};

// ADL finds this through `Optional_access_awaitable`, which lives in `ts::detail`.
template<typename R>
Optional_access_awaiter<R> operator co_await(Optional_access_awaitable<R> awaitable)
{
    return Optional_access_awaiter<R>(std::move(awaitable.core), awaitable.consumed);
}

// The awaiter build shared by both value categories: never-started check + core/consume hookup.
template<typename... Args>
Access_op_awaiter<typename Access_op<Args...>::result_type> make_access_op_awaiter(Access_op<Args...>& op)
{
#if TS_SAFETY_CHECKS
    if (!access_op_started(op))
        ts::fatal("co_await on an Access_op that was never started - start() it first "
                  "(awaiting a dormant op would suspend forever)");
#endif
    return Access_op_awaiter<typename Access_op<Args...>::result_type>(
        Task_ptr(access_op_core(op)), access_op_consumed(op));
}

} // namespace detail

// `co_await` an `Access_op` (usually the temporary from `obj.access(fn)`, materialized in the
// coroutine frame for the suspension - zero-alloc; or a named op, awaited once per cycle).
template<typename... Args>
auto operator co_await(Access_op<Args...>& op)
{
    return detail::make_access_op_awaiter(op);
}

template<typename... Args>
auto operator co_await(Access_op<Args...>&& op)
{
    return detail::make_access_op_awaiter(op);
}


// Async-lock a `Guarded<T>` in a coroutine: `auto g = co_await ts::read_write(w);` suspends
// until exclusive write access is granted, then resumes with an RAII guard giving direct
// `T&` (released on scope exit); `ts::read_only(w)` is the shared-reader form giving `const T&`.
// Linear RAII in place of a callback `async(fn, obj)`, and the safe shape as long as you do not
// `co_await` other work while the guard is alive (that holds the access across a suspension -
// faulted by the harness-as-suspension-detector). Deduces nothing; the mode is the verb.
template<typename T>
Access_awaiter<T, Access::read_write> read_write(Guarded<T>& w)
{
    return { global_scheduler(), detail::Guarded_access::pipe(w), detail::Guarded_access::instance(w) };
}

template<typename T>
Access_awaiter<T, Access::read_only> read_only(Guarded<T>& w)
{
    return { global_scheduler(), detail::Guarded_access::pipe(w), detail::Guarded_access::instance(w) };
}

// Multi-object scope access: hold grants on several `Guarded`s at once for one scope.
// `auto [x, y] = co_await ts::read_write(a, b);` acquires both in canonical pipe-address order
// (deadlock-free) and holds them until the guard leaves scope; the bindings give `a`/`b` by
// `T&` (write) or `const T&` (`read_only`). The two-plus-argument overloads sit beside the
// single-object ones above; a lone argument still yields the single-object `Access_guard`. v1
// is uniform-mode - use the callback `ts::access` when the objects need different modes.
template<typename T1, typename T2, typename... Ts>
Multi_access_awaiter<Access::read_write, T1, T2, Ts...>
read_write(Guarded<T1>& a, Guarded<T2>& b, Guarded<Ts>&... rest)
{
    return Multi_access_awaiter<Access::read_write, T1, T2, Ts...>(global_scheduler(), a, b, rest...);
}

template<typename T1, typename T2, typename... Ts>
Multi_access_awaiter<Access::read_only, T1, T2, Ts...>
read_only(Guarded<T1>& a, Guarded<T2>& b, Guarded<Ts>&... rest)
{
    return Multi_access_awaiter<Access::read_only, T1, T2, Ts...>(global_scheduler(), a, b, rest...);
}

} // namespace ts

// Tuple protocol for the multi-object guard, so structured bindings work:
//   auto [x, y] = co_await ts::read_write(a, b);
// `tuple_element` is the object type (const-qualified for read); the guard's member `get<I>()`
// returns the matching reference, so the bindings alias the live objects.
template<ts::Access Mode, typename... Ts>
struct std::tuple_size<ts::Multi_access_guard<Mode, Ts...>>
    : std::integral_constant<std::size_t, sizeof...(Ts)>
{
};

template<std::size_t I, ts::Access Mode, typename... Ts>
struct std::tuple_element<I, ts::Multi_access_guard<Mode, Ts...>>
{
    using element = std::tuple_element_t<I, std::tuple<Ts...>>;
    using type = std::conditional_t<Mode == ts::Access::read_write, element, const element>;
};

// A coroutine whose return type is `ts::Task<R>` uses `ts::detail::Task_promise<R>`.
template<typename R, typename... Args>
struct std::coroutine_traits<ts::Task<R>, Args...>
{
    using promise_type = ts::detail::Task_promise<R>;
};
