#pragma once

// Coroutine support -- makes `ts::Task<R>` awaitable and provides a `promise_type`
// so a coroutine can return `Task<R>` and `co_await` other tasks. Header-only, opt-in
// include (nothing else pulls it in). Guarded on `__cpp_impl_coroutine`, so a toolchain
// without coroutines simply sees an empty header (mandatory-coroutines is a later stage of
// docs/coroutine-first.md).
//
// Frame/block fusion (coroutine-first stage 1): the promise EMBEDS the task's
// `Task_control_block` (+ result storage) -- one allocation per coroutine task (the frame),
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
// installs `current_task = &core` for every SEGMENT of the body: the initial segment via
// the promise constructor (eager start runs the body immediately after), resumed segments
// via the resume trampoline; every genuine suspension restores the previous value before
// the thread leaves the frame (`exit_segment` in the awaiters, ordered before the
// suspension handshake publishes the frame for resumption -- no cross-thread overlap on
// the save slots). `ts::nested` inside any segment therefore attaches to the COROUTINE
// (its implicit scope), not to whatever task happened to launch it: the promise arms the
// block's `execution_flag` + self-lock exactly like `Executable::run`, the body-end drops
// the self-lock, and the last nested child completes the task. Children hold refs on the
// block, which keeps the frame -- and the coroutine's PARAMETERS -- alive until they
// settle; locals die at `co_return` as in any function, so children must not capture
// parent locals by reference past the join (`co_await ts::join_nested()` first).
//
// Resume scheduling: a resume goes through a bounded coroutine-resume trampoline
// (`schedule_resume`) on the settling/granting thread -- the eager-task equivalent of
// symmetric transfer (there is no suspended producer handle to transfer into; the awaited
// task runs to completion on its own thread, and the waiter's frame is resumed directly,
// iteratively, O(1) stack).

#include "ts/task.h"

#if defined(__cpp_impl_coroutine)

#include "ts/guarded.h"   // Pipe, pipe_acquire/release, Guarded(_access), global_scheduler

#include <atomic>
#include <coroutine>
#include <optional>
#include <utility>
#include <vector>

namespace ts
{
namespace detail
{

// Depth of live `Pipe_guard`s on THIS thread (the async-lock guards below). A guard is confined
// to one coroutine segment (the "no co_await under a guard" rule), so a thread-local count is
// right: any `co_await` that would actually suspend while a guard is held (`> 0`) is the
// lock-across-suspension anti-pattern and faults -- the harness doubling as a suspension
// detector. Incremented/decremented by `Pipe_guard`; checked in both `await_suspend`s.
inline thread_local int pipe_guard_depth = 0;

// Segment bracket, resolved per promise type: enter installs the coroutine's `current_task`
// (and, in the promise, its access snapshot is applied by the resume path); exit restores.
// A foreign promise (some other library's coroutine awaiting our task) has neither -- both
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
// granted the pipe -- whose ambient state is NOT the coroutine's. The scope's destructor
// only touches its saved `prev_`, so it stays valid even though `h.resume()` may destroy
// the frame when the coroutine completes (the segment state is restored by the awaiter /
// final awaiter BEFORE any destruction).
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
// (task.h) in spirit -- we can't reuse that vector because it is typed to `Task_ptr` and
// drives `block->execute()`, whereas here we drive a `coroutine_handle`. The first resume on a
// thread starts a drain; a resume requested DURING the drain (the cascade) is pushed and run by
// the outer loop, so the whole chain runs ITERATIVELY (O(1) stack). Type-erased to a thunk + one
// pointer -> NO per-resume allocation (the vector retains capacity across drains, like task.h's).
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
        return;   // an active drain on this thread will pick it up -- don't recurse
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
        return core_->ready.load(std::memory_order_acquire);
    }

    // Register the resume, resolving the synchronous-fire hazard: `attach` fires the callback
    // IMMEDIATELY if the block settled in the window since `await_ready` (closing the lost
    // wakeup). Were we to `h.resume()` inline there, the coroutine could run to completion and
    // destroy this frame (and this awaiter) while we are still inside `await_suspend` -- a
    // use-after-free. The handshake: whoever reaches `state_` first (the callback via
    // exchange(1), or the tail of `await_suspend` via exchange(2)) loses the resume; the
    // second one performs it.
    //
    // Segment ordering: `exit_segment` runs BEFORE the handshake publishes the frame for
    // resumption (the release-exchange), so a cross-thread resume's `enter_segment` never
    // overlaps this thread's save/restore of the segment slots; on the lost-exchange path
    // (synchronous fire) the segment is RE-ENTERED and the body continues uninterrupted.
    template<typename P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        // Suspension detector: reaching here means `await_ready` was false, so `co_await`ing
        // this task suspends the coroutine. Doing so while holding a `Pipe_guard` would hold
        // that pipe across the suspension (serialize / deadlock) -- the anti-pattern.
        if (pipe_guard_depth > 0)
            ts::fatal("co_await while holding a Guarded guard (pipe held across suspension)");

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

    Task_ptr core_;
    std::atomic<int> state_{ 0 };
};

// The shared (result-agnostic) half of the fused promise. The block is the FIRST member,
// so `Task_control_block* == promise*` (the `Executable` pattern) and the block's `destroy`
// destroys the whole coroutine frame. `num_locks` is armed to `execution_flag + 1`
// (executing + the body self-lock) in the constructor, so `ts::nested` attaches children to
// this coroutine across all its segments; the final awaiter drops the self-lock -- the task
// completes at `co_return` when no children are pending, else when the last child settles
// (the functor-node semantics of docs/coroutine-first.md §4.3).
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
    // The frame's implicit scope (docs/coroutine-first.md §4.3): children launched via
    // `ts::nested` in any segment are recorded here (in addition to the completion locks
    // they take), so `co_await ts::join_nested()` can await them mid-body. Same
    // single-thread-per-segment discipline as `prev_task_`.
    std::vector<Task_ptr> scope_children_;
    std::vector<Task_ptr>* prev_scope_ = nullptr;
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
    }

    void exit_segment()
    {
        current_task = std::move(prev_task_);
        current_scope_children = prev_scope_;
    }

    std::suspend_never initial_suspend() const noexcept { return {}; }

    // Final awaiter: restore the segment, drop the body self-lock (completing the task if no
    // nested children are pending -- otherwise the last child completes it), then release the
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

// Promise for a coroutine returning `Task<R>` -- the fused frame+block (see header comment).
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

// RAII async-lock guard over a `Guarded<T>`'s pipe, held for direct `T` access. Returned by
// `co_await ts::read_only(w)` / `ts::read_write(w)`. NON-COPYABLE AND NON-MOVABLE on purpose: it installs
// `current_access = &ctx_` (a member), so its address must be stable -- `await_resume` returns it
// as a prvalue and `auto g = co_await ...;` constructs it in place via guaranteed copy elision
// (a move would dangle the installed pointer; non-movable makes a stray copy a compile error).
// While the guard is alive `current_access` grants `obj_` in `Mode`, so `g->method()` passes the
// harness; the pipe is held (readers concurrent, writer exclusive) until the guard is destroyed.
template<typename T, Access Mode>
class Pipe_guard
{
public:
    Pipe_guard(Scheduler& scheduler, Pipe& pipe, T* obj)
        : scheduler_(scheduler)
        , pipe_(pipe)
        , obj_(obj)
    {
        if (current_access)
            ctx_ = *current_access;   // extend the coroutine's existing grant, don't replace it
        ctx_.add(obj_, Mode, detail::pipe_epoch(pipe_));
        prev_ = current_access;
        current_access = &ctx_;
        ++pipe_guard_depth;
    }

    ~Pipe_guard()
    {
        current_access = prev_;
        --pipe_guard_depth;
        pipe_release(scheduler_, pipe_, Mode);   // admit queued entries / the next guard
    }

    Pipe_guard(const Pipe_guard&) = delete;
    Pipe_guard& operator=(const Pipe_guard&) = delete;

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
    Pipe& pipe_;
    T* obj_;
    Access_context ctx_;
    const Access_context* prev_ = nullptr;
};

// Awaiter for `co_await ts::read_only(w)` / `ts::read_write(w)`. Acquires the pipe in `Mode` (holding it),
// then resumes with a `Pipe_guard`. The acquire/resume race (a deferred acquire's `on_acquired`
// firing on another thread vs `await_suspend` finishing) uses the same two-state handshake as
// `Task_awaiter`, with the same segment ordering.
template<typename T, Access Mode>
struct Pipe_guard_awaiter
{
    Pipe_guard_awaiter(Scheduler& scheduler, Pipe& pipe, T* obj) noexcept
        : scheduler_(scheduler)
        , pipe_(pipe)
        , obj_(obj)
    {}

    Pipe_guard_awaiter(const Pipe_guard_awaiter&) = delete;
    Pipe_guard_awaiter& operator=(const Pipe_guard_awaiter&) = delete;

    bool await_ready() const noexcept { return false; }   // must attempt the acquire (side effects)

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
        // a non-task thread) -- so `Deferred::commit` under a coroutine write guard can take
        // its held-grant fast path. `current_task` was restored by `exit_segment` above, so
        // name the coroutine's own core explicitly.
        Task_control_block* owner = owner_of(h);
        bool acquired = pipe_acquire(scheduler_, pipe_, Mode, std::move(on_acquired), owner);
        if (acquired)
        {
            enter_segment_if_ours(h.promise());
            return false;   // held now -> don't suspend; `await_resume` builds the guard
        }

        // Deferred: we are about to suspend. Suspending while holding another guard is the
        // lock-across-suspension anti-pattern.
        if (pipe_guard_depth > 0)
            ts::fatal("co_await a Guarded guard while holding another (pipe held across suspension)");

        if (state_.exchange(2, std::memory_order_acq_rel) == 1)
        {
            enter_segment_if_ours(h.promise());
            return false;   // on_acquired already fired -> resume via await_resume
        }
        return true;        // suspended; on_acquired will resume when the pipe grants
    }

    Pipe_guard<T, Mode> await_resume() noexcept
    {
        return Pipe_guard<T, Mode>(scheduler_, pipe_, obj_);   // prvalue -> elided into the local
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
};

// Awaiter joining a set of tasks: resumes once every one has settled. Shared by
// `ts::join_nested()` (the frame's implicit scope) and `Task_scope::join()`. Owns the
// handles; a countdown over the un-settled children plus the same two-state handshake as
// `Task_awaiter` (the last child's callback vs `await_suspend` finishing). `remaining_` is
// armed to the full count BEFORE any callback attaches, so a child settling mid-attach
// decrements early and the zero-transition still fires exactly once, on the true last.
struct Join_awaiter
{
    explicit Join_awaiter(std::vector<Task_ptr> children) noexcept
        : children_(std::move(children))
    {}

    Join_awaiter(const Join_awaiter&) = delete;
    Join_awaiter& operator=(const Join_awaiter&) = delete;

    bool await_ready()
    {
        std::erase_if(children_, [](const Task_ptr& c)
        {
            return c->ready.load(std::memory_order_acquire);
        });
        remaining_.store(static_cast<int>(children_.size()), std::memory_order_relaxed);
        return children_.empty();
    }

    template<typename P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        if (pipe_guard_depth > 0)
            ts::fatal("co_await while holding a Guarded guard (pipe held across suspension)");

        exit_segment_if_ours(h.promise());

        for (Task_ptr& c : children_)
        {
            c->attach([this, h](void*, bool)
            {
                if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1
                    && state_.exchange(1, std::memory_order_acq_rel) == 2)
                    schedule_resume(h);
            });
        }

        if (state_.exchange(2, std::memory_order_acq_rel) == 1)
        {
            enter_segment_if_ours(h.promise());
            return false;   // the last child settled synchronously during the attach loop
        }
        return true;
    }

    void await_resume() const noexcept {}

    std::vector<Task_ptr> children_;
    std::atomic<int> remaining_{ 0 };
    std::atomic<int> state_{ 0 };
};

} // namespace detail

// Mid-body join of the implicit scope (docs/coroutine-first.md §4.3): awaits every child
// launched so far via `ts::nested` in this coroutine, then resumes; the list resets, so
// later launches join a later `join_nested` (or gate `co_return` via the counter as usual).
// Outside a coroutine frame there is no implicit scope and the await is a no-op.
inline detail::Join_awaiter join_nested()
{
    std::vector<detail::Task_ptr> children;
    if (detail::current_scope_children != nullptr)
    {
        children = std::move(*detail::current_scope_children);
        detail::current_scope_children->clear();
    }
    return detail::Join_awaiter(std::move(children));
}

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
// until the pipe grants exclusive write access, then resumes with an RAII guard giving direct
// `T&` (released on scope exit); `ts::read_only(w)` is the shared-reader form giving `const T&`.
// Linear RAII in place of a callback `async(fn, obj)`, and the safe shape as long as you do NOT
// `co_await` other work while the guard is alive (that holds the pipe across a suspension --
// faulted by the harness-as-suspension-detector). Deduces nothing; the mode is the verb.
template<typename T>
detail::Pipe_guard_awaiter<T, Access::read_write> read_write(Guarded<T>& w)
{
    return { global_scheduler(), detail::Guarded_access::pipe(w), detail::Guarded_access::instance(w) };
}

template<typename T>
detail::Pipe_guard_awaiter<T, Access::read_only> read_only(Guarded<T>& w)
{
    return { global_scheduler(), detail::Guarded_access::pipe(w), detail::Guarded_access::instance(w) };
}

} // namespace ts

// A coroutine whose return type is `ts::Task<R>` uses `ts::detail::Task_promise<R>`.
template<typename R, typename... Args>
struct std::coroutine_traits<ts::Task<R>, Args...>
{
    using promise_type = ts::detail::Task_promise<R>;
};

#endif // __cpp_impl_coroutine
