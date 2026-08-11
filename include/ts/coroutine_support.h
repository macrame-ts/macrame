#pragma once

// Coroutine support - makes `ts::Task<R>` awaitable and provides a `promise_type`
// so a coroutine can return `Task<R>` and `co_await` other tasks. Header-only;
// coroutines are mandatory (docs/coroutine-first.md), so the library assumes a
// coroutine-capable toolchain.
//
// Frame/block fusion (coroutine-first stage 1): the promise embeds the task's
// `Task_control_block` (+ result storage) - one allocation per coroutine task (the frame),
// not frame + block. The block is the promise's first member, so the block pointer doubles
// as the promise pointer (the `Executable` first-member pattern); the block's `destroy`
// thunk destroys the whole frame via `coroutine_handle::from_promise`. Lifetime: the
// promise holds a "running" self-reference released at `final_suspend`; the returned
// `Task<R>`, awaiters, and nested children hold their own refs, so the frame (and the
// result inside it) outlives the last observer, and a fire-and-forget coroutine whose
// handle is dropped stays alive until it completes.
//
// Segment-carried ambient state: a coroutine migrates threads across suspension, but the
// harness and the nested-task machinery key off thread-locals (`current_access`,
// `current_task`). The promise snapshots the creator's grant once (`snapshot_access`) and
// installs `current_task = &core` for every segment of the body: the initial segment via
// the promise constructor (eager start runs the body immediately after), resumed segments
// via the resume trampoline; every genuine suspension restores the previous value before
// the thread leaves the frame (`exit_segment` in the awaiters, ordered before the
// suspension handshake publishes the frame for resumption - no cross-thread overlap on
// the save slots). A nested graph run started inside any segment therefore attaches to the
// coroutine (via `detail::add_nested`), not to whatever task happened to launch it: the
// promise arms the block's `execution_flag` + self-lock exactly like `Executable::run`, the
// body-end drops the self-lock, and the last nested completion completes the task. A gating
// child holds a ref on the block, which keeps the frame - and the coroutine's parameters -
// alive until it settles.
//
// Resume scheduling: a resume goes through a bounded coroutine-resume trampoline
// (`schedule_resume`) on the settling/granting thread - the eager-task equivalent of
// symmetric transfer (there is no suspended producer handle to transfer into; the awaited
// task runs to completion on its own thread, and the waiter's frame is resumed directly,
// iteratively, O(1) stack).

#include "ts/task.h"
#include "ts/guarded.h"   // Pipe, pipe_acquire/release, Guarded(_access), global_scheduler
#include "ts/detail/suspension_registry.h"   // tier 3 of the deadlock report

#include <atomic>
#include <coroutine>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace ts
{
namespace detail
{

// Depth of live `Access_guard`s on this thread (the async-lock guards below). A guard is confined
// to one coroutine segment (the "no co_await under a guard" rule), so a thread-local count is
// right: any `co_await` that would actually suspend while a guard is held (`> 0`) is the
// lock-across-suspension anti-pattern and faults - the harness doubling as a suspension
// detector. Incremented/decremented by `Access_guard`; checked in both `await_suspend`s.
//
// `Rule::await_under_guard` (ts/rules.h) is a structural rule: it has no scoped opt-out,
// because a guard that did survive a suspension would leave the resumed segment installing
// the promise's access snapshot over the guard's own context, and the guard's destructor
// restoring a `current_access` pointer captured on another thread. The rule protects an
// implementation invariant, not merely a user-level hazard. It can be compiled out
// wholesale (`TS_ENABLED_RULES`), which also removes the counter.
#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
inline thread_local int access_guard_depth = 0;
#endif

// The guard-across-suspension check. It lives at `co_await` entry (`await_ready`), not at
// the suspension (TODO 6.11): a `co_await` that happens to complete synchronously is just
// as illegal as one that suspends, and gating on "did it actually suspend" made the check
// fire only on the runs where timing was unfriendly - so a hold-then-await shipped
// undiagnosed whenever the target pipe was momentarily free.
//
// The reentrancy exemption used to be emergent (a reentrant same-object access never
// suspends, so it never reached the check). Hoisting forces it to be stated: see
// `reentrant_under_held_grant` below. Everything else is illegal, guard depth > 0.
inline void check_await_under_guard(const char* message)
{
#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
    if (access_guard_depth > 0 && rule_enforced(Rule::await_under_guard))
        ts::fatal(message);
#else
    (void)message;
#endif
}

// The one stated exemption to the rule above. An access whose every pipe is currently
// write-owned by this task ran inline under that held grant (`Guarded::access`'s reentrant
// arm, waiting rule (b)): it never touched the pipe, it is settled before the `co_await`
// is even evaluated, and it cannot suspend by construction rather than by timing.
// `writer_owner` is always-on state, so this works in every build.
//
// Deliberately narrow. A read access under a read guard is not exempt: it reaches the pipe,
// and if a writer is queued ahead it enqueues and suspends - with our own read hold
// blocking that writer. That is the genuine hazard, not a false positive.
inline bool reentrant_under_held_grant(const Task_control_block* blk) noexcept
{
    if (blk == nullptr || blk->pipe_count == 0)
        return false;
    for (std::uint8_t i = 0; i < blk->pipe_count; ++i)
    {
        const Pipe* pipe = blk->pipe_links[i].pipe;
        if (pipe == nullptr || pipe->writer_owner.load(std::memory_order_acquire) != current_task.get())
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
    if (pipe == nullptr || current_access == nullptr || !rule_enforced(Rule::access_rank))
        return;
    if (instance != nullptr && current_access->check(instance, Access::read_only) != Access_context::Grant::none)
        return;   // already held - not a later acquisition
    const Access_context::Rank_state held = current_access->rank_state();
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

// Segment bracket, resolved per promise type: enter installs the coroutine's `current_task`
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

// Resume a coroutine, re-installing its captured access grant and its `current_task`
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
// A), whose completion resumes C, ... This mirrors `Task_control_block::inline_pending`
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
inline thread_local std::vector<Resume_item> resume_pending;
inline thread_local bool resume_draining = false;

template<typename P>
void resume_thunk(void* addr)
{
    resume_with_access(std::coroutine_handle<P>::from_address(addr));
}

template<typename P>
void schedule_resume(std::coroutine_handle<P> h)
{
    resume_pending.push_back({ &resume_thunk<P>, h.address() });
    if (resume_draining)
        return;   // an active drain on this thread will pick it up - don't recurse
    resume_draining = true;
    for (std::size_t head = 0; head < resume_pending.size(); ++head)
    {
        Resume_item item = resume_pending[head];   // copy: a nested push may realloc the vector
        item.thunk(item.handle);                   // re-install ambient state + h.resume()
    }
    resume_pending.clear();   // retains capacity -> no steady-state allocation
    resume_draining = false;
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
        if (core_->pipe_count > 0 && current_access != nullptr && rule_enforced(Rule::circular_wait))
        {
            Pipe* awaited[Access_context::max_entries];   // sized to the object cap, so it never truncates
            int n = 0;
            for (std::uint8_t i = 0; i < core_->pipe_count && n < Access_context::max_entries; ++i)
                awaited[n++] = core_->pipe_links[i].pipe;
            recorded_ = circular_wait_record(current_access, this, current_task.get(), awaited, n);
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

// The shared (result-agnostic) half of the fused promise. The block is the first member,
// so `Task_control_block* == promise*` (the `Executable` pattern) and the block's `destroy`
// destroys the whole coroutine frame. `num_locks` is armed to `execution_flag + 1`
// (executing + the body self-lock) in the constructor, so `detail::add_nested` can attach a
// gating child (a nested graph run) to this coroutine across all its segments; the final
// awaiter drops the self-lock - the task completes at `co_return` when no children are
// pending, else when the last child settles.
template<typename Derived>
struct Promise_base
{
    Task_control_block core;   // MUST be first: block pointer doubles as promise pointer

    // The ambient access grant at creation (empty if created outside any task), re-installed
    // around each resumed segment.
    std::optional<Access_context> access_ctx_ = snapshot_access();
    // Saved `current_task` of the enclosing segment; valid only while this coroutine's
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
    // The coroutine's dispatch priority, carried onto the block for queued uses of its task.
    Priority priority_ = Priority::normal;

    Promise_base()
    {
        core.destroy = &destroy_frame;
        // The "running" self-reference: keeps the frame alive while the body runs even if
        // every external handle is dropped (fire-and-forget). Released by the final awaiter.
        core.refcount.store(1, std::memory_order_relaxed);
        // Executing + body self-lock: nested children add completion locks (task.h §4 regime).
        core.num_locks.store(Task_control_block::execution_flag + 1, std::memory_order_relaxed);
        core.flags.priority = priority_;
        inherit_task_name(core);   // before `enter_segment` installs this frame as current
        enter_segment();   // the eager body runs on the caller right after the promise ctor
    }

    static void destroy_frame(Task_control_block* c)
    {
        auto& promise = *reinterpret_cast<Derived*>(c);
        std::coroutine_handle<Derived>::from_promise(promise).destroy();
    }

    void enter_segment()
    {
        prev_task_ = std::move(current_task);
        current_task = Task_ptr(&core);
        prev_scope_ = current_scope_children;
        current_scope_children = &scope_children_;
        relaxed_.enter();
    }

    void exit_segment()
    {
        relaxed_.exit();
        current_task = std::move(prev_task_);
        current_scope_children = prev_scope_;
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
            Task_control_block* c = &promise.core;
            if (c->num_locks.fetch_sub(1, std::memory_order_acq_rel) == Task_control_block::execution_flag + 1)
                c->complete();
            intrusive_dec(c);   // may destroy the frame; touch nothing afterwards
        }

        void await_resume() const noexcept {}   // never resumed
    };

    Final_awaiter final_suspend() noexcept { return {}; }

    void unhandled_exception()
    {
        ts::fatal("coroutine body escaped an exception (exceptions are disabled project-wide)");
    }
};

// Promise for a coroutine returning `Task<R>` - the fused frame+block (see header comment).
template<typename R>
struct Task_promise : Promise_base<Task_promise<R>>
{
    Result_storage<R> storage;

    Task<R> get_return_object() { return Task<R>(Task_ptr(&this->core)); }

    void return_value(R value)
    {
        storage.result.emplace(std::move(value));
        this->core.result_ptr = &*storage.result;
        // Completion happens in the final awaiter (after the self-lock drop), uniform with
        // the nested-children gate.
    }
};

template<>
struct Task_promise<void> : Promise_base<Task_promise<void>>
{
    Task<void> get_return_object() { return Task<void>(Task_ptr(&core)); }

    void return_void() {}   // completion happens in the final awaiter
};

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
    Access_guard(Scheduler& scheduler, detail::Pipe& pipe, T* obj)
        : scheduler_(scheduler)
        , pipe_(pipe)
        , obj_(obj)
    {
        if (detail::current_access)
            ctx_ = *detail::current_access;   // extend the coroutine's existing grant, don't replace it
        ctx_.add(obj_, Mode, detail::pipe_epoch(pipe_), detail::pipe_rank(pipe_));
        prev_ = detail::current_access;
        detail::current_access = &ctx_;
#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
        ++detail::access_guard_depth;
#endif
    }

    ~Access_guard()
    {
        detail::current_access = prev_;
#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
        --detail::access_guard_depth;
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

    Multi_access_guard(Scheduler& scheduler, std::tuple<Guarded<Ts>*...> objs,
                       detail::Pipe* const* pipes, std::size_t pipe_count)
        : scheduler_(scheduler)
        , objs_(objs)
        , pipe_count_(pipe_count)
    {
        for (std::size_t i = 0; i < pipe_count_; ++i)
            pipes_[i] = pipes[i];
        if (detail::current_access)
            ctx_ = *detail::current_access;   // extend the coroutine's existing grant
        add_all(std::index_sequence_for<Ts...>{});
        prev_ = detail::current_access;
        detail::current_access = &ctx_;
#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
        ++detail::access_guard_depth;
#endif
    }

    ~Multi_access_guard()
    {
        detail::current_access = prev_;
#if TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)
        --detail::access_guard_depth;
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
        // its held-grant fast path. `current_task` was restored by `exit_segment` above, so
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
            recorded_ = circular_wait_record(current_access, this, owner_of(h), &awaited, 1);
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
    template<typename P>
    static Task_control_block* owner_of(std::coroutine_handle<P> h) noexcept
    {
        if constexpr (requires { h.promise().core; })
            return &h.promise().core;
        else
            return current_task.get();
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
        // Insertion-sort by pipe address + dedup (uniform mode - a repeated object is simply
        // dropped), the canonical order `async_build_modes` uses. `pipe_count_` <= N.
        for (std::size_t k = 0; k < N; ++k)
        {
            Pipe* pk = raw[k];
            std::size_t i = 0;
            while (i < pipe_count_ && pipes_[i] < pk)
                ++i;
            if (i < pipe_count_ && pipes_[i] == pk)
                continue;
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
        held_ctx_ = current_access;   // the coroutine's grants, captured for the wait record
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

    template<typename P>
    static Task_control_block* owner_of(std::coroutine_handle<P> h) noexcept
    {
        if constexpr (requires { h.promise().core; })
            return &h.promise().core;
        else
            return current_task.get();
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

// `co_await task` -> suspend until `task` settles, then resume with its result (`const R&`,
// non-consuming; `void` for a void task). ADL finds these in namespace `ts`.
template<typename R>
detail::Task_awaiter<R> operator co_await(const Task<R>& t)
{
    return detail::Task_awaiter<R>(detail::core_of(t));
}

template<typename R>
detail::Task_awaiter<R> operator co_await(Task<R>&& t)
{
    return detail::Task_awaiter<R>(detail::core_of(t));
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
