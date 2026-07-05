#pragma once

// Coroutine support SPIKE -- makes `ts::Task<R>` awaitable and provides a `promise_type`
// so a coroutine can return `Task<R>` and `co_await` other tasks. Header-only, opt-in
// include (nothing else pulls it in). Guarded on `__cpp_impl_coroutine`, so a toolchain
// without coroutines simply sees an empty header. No edits to task.h: the awaiter reaches
// the block through the existing `detail::core_of` friend, and the return type is wired via
// a `std::coroutine_traits` specialization.
//
// FOLLOW-UPS (deliberately out of scope for the spike -- see docs/TODO.md "Coroutines"):
//  1. Per-segment `Access_context` re-install. A coroutine migrates threads across
//     suspension, but the access harness uses `thread_local current_access`. Each resumed
//     segment must re-install the coroutine's `Access_context` (reuse the nested-task
//     inheritance machinery: `snapshot_access` + `Inherited_access_scope`). Right now a
//     resumed segment runs with NO access grant, so a resumed body touching guarded data
//     would fault the harness. The tests below never touch guarded data across a suspension.
//  2. `Thread_safe` async-lock guard: `auto g = co_await w.write();` -> suspend until the
//     pipe grants, resume with an RAII guard over `T`, release on scope exit. Plus the
//     harness-as-suspension-detector: fault if a pipe grant is held across a suspension.
//  3. Resume scheduling. `await_suspend` resumes INLINE on the thread that settles the
//     awaited task; a production version should schedule the resume as a task-segment (via
//     the scheduler / inline trampoline) carrying the coroutine's priority + access context.

#include "task.h"

#if defined(__cpp_impl_coroutine)

#include <atomic>
#include <coroutine>
#include <utility>

namespace ts
{
namespace detail
{

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
    bool await_suspend(std::coroutine_handle<> h)
    {
        core_->attach([this, h](void*, bool)
        {
            if (state_.exchange(1, std::memory_order_acq_rel) == 2)
                h.resume();   // await_suspend already suspended -> we own the resume
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
};

template<>
struct Task_promise<void>
{
    Task_promise()
        : core_(make_bare_block())
    {}

    Task<void> get_return_object() { return Task<void>(core_); }
    std::suspend_never initial_suspend() const noexcept { return {}; }
    std::suspend_never final_suspend() const noexcept { return {}; }

    void return_void() { core_->complete(); }

    void unhandled_exception()
    {
        ts::fatal("coroutine body escaped an exception (exceptions are disabled project-wide)");
    }

    Task_ptr core_;
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

} // namespace ts

// A coroutine whose return type is `ts::Task<R>` uses `ts::detail::Task_promise<R>`.
template<typename R, typename... Args>
struct std::coroutine_traits<ts::Task<R>, Args...>
{
    using promise_type = ts::detail::Task_promise<R>;
};

#endif // __cpp_impl_coroutine
