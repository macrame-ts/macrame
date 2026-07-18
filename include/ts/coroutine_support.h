#pragma once

// Coroutine support -- makes `ts::Task<R>` awaitable and provides a `promise_type`
// so a coroutine can return `Task<R>` and `co_await` other tasks. Header-only, opt-in
// include (nothing else pulls it in). Guarded on `__cpp_impl_coroutine`, so a toolchain
// without coroutines simply sees an empty header. No edits to task.h: the awaiter reaches
// the block through the existing `detail::core_of` friend, and the return type is wired via
// a `std::coroutine_traits` specialization.
//
// Per-segment `Access_context` re-install: a coroutine migrates threads across suspension,
// but the harness keys off `thread_local current_access`, so the promise snapshots the
// ambient grant at creation (`snapshot_access`, the same value the launcher would inherit)
// and each resumed segment re-installs it (`Inherited_access_scope` around the resume) --
// a resumed body may touch data the coroutine was granted, harness intact.
//
// The `Guarded` async-lock guard: `auto g = co_await ts::read_write(w);` (or
// `ts::read_only(w)`) suspends until the pipe grants, resumes with an RAII `Pipe_guard`
// giving direct `T&`/`const T&`, released on scope exit. The harness doubles as a suspension
// detector: a `co_await` that would suspend while a guard is held faults (`pipe_guard_depth`).
//
// Resume scheduling: a resume goes through a bounded coroutine-resume trampoline
// (`schedule_resume`), mirroring `Task_control_block::inline_pending`, rather than recursing
// via inline `h.resume()` -- a deep cascade of coroutine completions runs iteratively
// (O(1) stack) instead of overflowing. The resume stays on the settling thread (no
// queue hop -> lowest latency); the coroutine's `priority_` is carried onto its block for the
// queued paths.

#include "ts/task.h"

#if defined(__cpp_impl_coroutine)

#include "ts/guarded.h"   // Pipe, pipe_acquire/release, Guarded(_access), default_scheduler

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

// Resume a coroutine, re-installing its captured access grant for the resumed segment. The
// async resume runs on the thread that settled the awaited task / granted the pipe -- whose
// `current_access` is NOT the coroutine's -- so re-install `Task_promise::access_ctx_` (a
// `snapshot_access()` copy taken at creation) around `h.resume()`, same mechanism as
// nested-task inheritance. The `requires` gate keeps `co_await` usable inside ANY coroutine (a
// promise without `access_ctx_` just resumes plainly). The scope's destructor only touches its
// saved `prev_`, so it stays valid even though `h.resume()` may destroy the frame (and
// `access_ctx_`) when the coroutine completes.
template<typename P>
inline void resume_with_access(std::coroutine_handle<P> h)
{
    if constexpr (requires { h.promise().access_ctx_; })
    {
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
// A), whose completion resumes C, ... -- i.e. settle -> awaiter callback -> resume -> return ->
// complete -> settle -> ... nested per level. This mirrors `Task_control_block::inline_pending`
// (task.h) EXACTLY in spirit -- we can't reuse that vector because it is typed to `Task_ptr` and
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
        item.thunk(item.handle);                   // re-install access + h.resume() (may destroy the frame)
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
    // second one performs it. So a synchronous fire returns `false` (don't suspend -> the
    // machinery calls `await_resume`), while an async fire (the common case, on the settling
    // worker) resumes after we have fully suspended. `state_` lives in the frame, which
    // outlives the suspension, so it is valid when a later cross-thread callback reads it.
    template<typename P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        // Suspension detector: reaching here means `await_ready` was false, so `co_await`ing
        // this task suspends the coroutine. Doing so while holding a `Pipe_guard` would hold
        // that pipe across the suspension (serialize / deadlock) -- the anti-pattern.
        if (pipe_guard_depth > 0)
            ts::fatal("co_await while holding a Guarded guard (pipe held across suspension)");

        core_->attach([this, h](void*, bool)
        {
            if (state_.exchange(1, std::memory_order_acq_rel) == 2)
                schedule_resume(h);   // await_suspend already suspended -> we own the resume (via the bounded trampoline)
        });

        if (state_.exchange(2, std::memory_order_acq_rel) == 1)
            return false;     // callback already fired synchronously -> resume via await_resume
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
            return *static_cast<const R*>(core_->result_ptr);   // const R&, non-consuming (see 1.6)
        }
    }

    Task_ptr core_;
    std::atomic<int> state_{ 0 };
};

// Promise for a coroutine returning `Task<R>`. The result block is DECOUPLED from the
// coroutine frame -- a separately heap-allocated, intrusively-refcounted `Result_block<R>`
// (or a bare block for void) co-owned by the returned `Task<R>` handle. So it is safe for
// `final_suspend` to self-destroy the frame (`suspend_never`): `return_value`/`return_void`
// has already stored the result into the block and `complete()`d it (which resumes awaiters
// FORWARD, into a different coroutine -- never re-entering this frame). `initial_suspend` is
// `suspend_never` -> eager start (the body runs on the caller up to the first suspending
// `co_await`, like `ts::launch`). `unhandled_exception` is unreachable (`_HAS_EXCEPTIONS=0`)
// but must exist.
template<typename R>
struct Task_promise
{
    Task_promise()
    {
        auto [c, w] = make_block<R>();
        core_ = std::move(c);
        wrapper_ = w;
        core_->flags.priority = priority_;   // carry the coroutine's dispatch priority onto its block
    }

    Task<R> get_return_object() { return Task<R>(core_); }
    std::suspend_never initial_suspend() const noexcept { return {}; }
    std::suspend_never final_suspend() const noexcept { return {}; }

    void return_value(R value)
    {
        wrapper_->store(std::move(value));
        core_->complete();
    }

    void unhandled_exception()
    {
        ts::fatal("coroutine body escaped an exception (exceptions are disabled project-wide)");
    }

    Task_ptr core_;
    Result_block<R>* wrapper_ = nullptr;
    // The ambient access grant at creation (empty if created outside any task). Re-installed
    // around each resumed segment so the harness passes across thread migration. Snapshotted
    // here (member init runs in the promise ctor, on the creating thread, before the body).
    std::optional<Access_context> access_ctx_ = snapshot_access();
    // The coroutine's dispatch priority, carried onto its block (`core_->flags.priority`) so a
    // QUEUED use of the coroutine's `Task` (as a prerequisite / continuation) respects it. Default
    // `normal`; there is no setter yet (a coroutine has no config channel), and the resumed segment
    // runs inline via the bounded trampoline (no queue hop), so a resume itself carries no queue
    // priority -- the field is wired for the queued paths and a future queued-resume variant.
    Priority priority_ = Priority::normal;
};

template<>
struct Task_promise<void>
{
    Task_promise()
        : core_(make_bare_block())
    {
        core_->flags.priority = priority_;   // see Task_promise<R>
    }

    Task<void> get_return_object() { return Task<void>(core_); }
    std::suspend_never initial_suspend() const noexcept { return {}; }
    std::suspend_never final_suspend() const noexcept { return {}; }

    void return_void() { core_->complete(); }

    void unhandled_exception()
    {
        ts::fatal("coroutine body escaped an exception (exceptions are disabled project-wide)");
    }

    Task_ptr core_;
    std::optional<Access_context> access_ctx_ = snapshot_access();   // see Task_promise<R>
    Priority priority_ = Priority::normal;                           // see Task_promise<R>
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
        ctx_.add(obj_, Mode);
        prev_ = current_access;
        current_access = &ctx_;
        ++pipe_guard_depth;
    }

    ~Pipe_guard()
    {
        current_access = prev_;
        --pipe_guard_depth;
        pipe_release(scheduler_, pipe_, Mode);   // admit queued jobs / the next guard
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
// `Task_awaiter`.
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
        // Try to acquire now; `pipe_acquire` returns true (held, no callback) or false (deferred,
        // `on_acquired` fires once when the pipe drains to us, possibly on another thread).
        bool acquired = pipe_acquire(scheduler_, pipe_, Mode,
            [this, h]
            {
                if (state_.exchange(1, std::memory_order_acq_rel) == 2)
                    schedule_resume(h);   // re-install grant + resume (via the bounded trampoline)
            });
        if (acquired)
            return false;   // held now -> don't suspend; `await_resume` builds the guard

        // Deferred: we are about to suspend. Suspending while holding another guard is the
        // lock-across-suspension anti-pattern.
        if (pipe_guard_depth > 0)
            ts::fatal("co_await a Guarded guard while holding another (pipe held across suspension)");

        if (state_.exchange(2, std::memory_order_acq_rel) == 1)
            return false;   // on_acquired already fired -> resume via await_resume
        return true;        // suspended; on_acquired will resume when the pipe grants
    }

    Pipe_guard<T, Mode> await_resume() noexcept
    {
        return Pipe_guard<T, Mode>(scheduler_, pipe_, obj_);   // prvalue -> elided into the local
    }

    Scheduler& scheduler_;
    Pipe& pipe_;
    T* obj_;
    std::atomic<int> state_{ 0 };
};

} // namespace detail

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
    return { default_scheduler(), detail::Guarded_access::pipe(w), detail::Guarded_access::instance(w) };
}

template<typename T>
detail::Pipe_guard_awaiter<T, Access::read_only> read_only(Guarded<T>& w)
{
    return { default_scheduler(), detail::Guarded_access::pipe(w), detail::Guarded_access::instance(w) };
}

} // namespace ts

// A coroutine whose return type is `ts::Task<R>` uses `ts::detail::Task_promise<R>`.
template<typename R, typename... Args>
struct std::coroutine_traits<ts::Task<R>, Args...>
{
    using promise_type = ts::detail::Task_promise<R>;
};

#endif // __cpp_impl_coroutine
