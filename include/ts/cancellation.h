#pragma once

// Cooperative cancellation. A `Cancellation_source` owns the request flag; hand its
// `token()` to `async`/`launch`/`Static_task_graph::execute`. Cancellation is checked
// when a task/node is about to run (not-yet-started work is skipped) and propagates
// down awaiting coroutines and graph successors as a completion state (see
// `Task::is_cancelled`). A default-constructed token is never cancelled. For a *push*
// notification (wake work that blocks rather than polls) register a `Cancel_callback`
// on the token - the RAII registration, `std::stop_callback`-shaped.
// User guide: docs/guide.md §4.4.

#include "ts/detail/ref_count.h"   // intrusive Ref_ptr / Ref_counted (preferred over shared_ptr)

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ts
{

class Cancel_callback;

namespace detail
{

// Shared cancellation state behind a source / its tokens / its callbacks. The request
// flag is atomic so the hot `is_cancel_requested()` needs no lock; the callback list
// (fired by `request_cancel`) is mutex-guarded, with `firing`/`firing_thread`/`done` for
// the teardown race (a `Cancel_callback` destroyed while it is being invoked).
struct Cancel_state : Ref_counted<Cancel_state>
{
    std::atomic<bool> requested{ false };
    std::mutex mutex;
    std::vector<Cancel_callback*> callbacks;   // registered, not yet fired
    Cancel_callback* firing = nullptr;         // callback currently being invoked, if any
    std::thread::id firing_thread{};
    std::condition_variable done;              // notified when `firing` finishes
};

} // namespace detail

class Cancellation_token
{
public:
    Cancellation_token() = default;

    bool is_cancel_requested() const noexcept
    {
        return state_ && state_->requested.load(std::memory_order_acquire);
    }

private:
    friend class Cancellation_source;
    friend class Cancel_callback;

    explicit Cancellation_token(detail::Ref_ptr<detail::Cancel_state> state) noexcept
        : state_(std::move(state))
    {}

    detail::Ref_ptr<detail::Cancel_state> state_;
};

class Cancellation_source
{
public:
    Cancellation_source()
        : state_(detail::make_ref<detail::Cancel_state>())
    {}

    // Request cancellation and fire every registered `Cancel_callback` synchronously, on
    // this thread. Idempotent - the first call wins; later calls (and callbacks registered
    // after) are no-ops / fire immediately.
    void request_cancel();

    bool is_cancel_requested() const noexcept { return state_->requested.load(std::memory_order_acquire); }
    Cancellation_token token() const noexcept { return Cancellation_token(state_); }

private:
    detail::Ref_ptr<detail::Cancel_state> state_;
};

// RAII push notification: registers `fn` on `token`; `request_cancel()` invokes it. If
// cancellation was already requested at construction, `fn` runs now, in the constructor.
// The destructor deregisters - and if the callback is mid-invocation on another thread it
// waits for that to finish (so `fn`'s captures stay valid), except when the callback is
// destroying itself re-entrantly (then it detaches, to avoid deadlock). Non-copyable,
// non-movable (its address is the registration identity), like `std::stop_callback`.
class Cancel_callback
{
public:
    template<typename Fn>
    Cancel_callback(const Cancellation_token& token, Fn&& fn)
        : state_(token.state_)
        , fn_(std::forward<Fn>(fn))
    {
        if (!state_)
            return;   // token never cancels
        std::unique_lock lock(state_->mutex);
        if (state_->requested.load(std::memory_order_relaxed))
        {
            lock.unlock();
            fn_();   // already requested -> fire now, on this thread
        }
        else
        {
            state_->callbacks.push_back(this);
        }
    }

    ~Cancel_callback()
    {
        if (!state_)
            return;
        std::unique_lock lock(state_->mutex);
        for (auto it = state_->callbacks.begin(); it != state_->callbacks.end(); ++it)
        {
            if (*it == this)
            {
                state_->callbacks.erase(it);   // not yet fired -> just deregister
                return;
            }
        }
        if (state_->firing == this)            // mid-invocation
        {
            if (state_->firing_thread == std::this_thread::get_id())
                // Re-entrant self-destroy (the callback deleted its own `Cancel_callback`):
                // detach, because waiting for `firing` to clear would deadlock on ourselves.
                // This destroys `fn_` while its own invocation is still on the stack - the
                // `std::stop_callback` shape - which is sound because `move_only_function`
                // does not touch the target after it returns.
                return;
            state_->done.wait(lock, [&] { return state_->firing != this; });
        }
    }

    Cancel_callback(const Cancel_callback&) = delete;
    Cancel_callback& operator=(const Cancel_callback&) = delete;

private:
    friend class Cancellation_source;

    detail::Ref_ptr<detail::Cancel_state> state_;
    std::move_only_function<void()> fn_;
};

inline void Cancellation_source::request_cancel()
{
    if (!state_)
        return;
    std::unique_lock lock(state_->mutex);
    if (state_->requested.exchange(true, std::memory_order_release))
        return;   // already requested
    while (!state_->callbacks.empty())
    {
        Cancel_callback* cb = state_->callbacks.back();
        state_->callbacks.pop_back();
        state_->firing = cb;
        state_->firing_thread = std::this_thread::get_id();
        lock.unlock();
        cb->fn_();               // run outside the lock (may re-enter / register more)
        lock.lock();
        state_->firing = nullptr;
        state_->done.notify_all();
    }
}

} // namespace ts
