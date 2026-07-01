#pragma once

#include "fatal.h"

#include <array>
#include <atomic>
#include <concepts>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ts
{

// Cooperative cancellation. A `Cancellation_source` owns the request flag; hand its
// `token()` to `async`/`then`/`Static_task_graph::execute`. Cancellation is checked
// when a task/node is about to run (not-yet-started work is skipped) and propagates
// down continuations and graph successors as a completion state (see `is_cancelled`).
// A default-constructed token is never cancelled.
class Cancellation_token
{
public:
    Cancellation_token() = default;

    explicit Cancellation_token(std::shared_ptr<std::atomic<bool>> flag) noexcept
        : flag_(std::move(flag))
    {}

    bool is_cancel_requested() const noexcept
    {
        return flag_ && flag_->load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

class Cancellation_source
{
public:
    Cancellation_source()
        : flag_(std::make_shared<std::atomic<bool>>(false))
    {}

    void request_cancel() noexcept { flag_->store(true, std::memory_order_release); }
    bool is_cancel_requested() const noexcept { return flag_->load(std::memory_order_acquire); }
    Cancellation_token token() const noexcept { return Cancellation_token(flag_); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

namespace detail
{

// --- Task control block ----------------------------------------------------
//
// The refcounted completion/dependency core behind a `Task<R>` handle. Holds the
// result plus a list of continuations attached via `Task::then()`. Continuations
// fire when the producer completes (inline on the completing worker); a
// continuation attached after completion runs inline on the caller. Monomorphic on
// `R` only — the closure type is erased at the entry point (`Thread_safe::launch`)
// and never reaches here, so there is no per-closure instantiation, no vtable, and
// no virtual dispatch (see docs/task-internals.md §2).
// Note: mixing `get()` and `then()` on the same task, or calling `get()` twice, is
// unsupported (`get()` moves the result out). `complete()` is idempotent so a
// bodyless block can be triggered (see `Signal`); the first completion wins.

template<typename R>
struct Task_control_block
{
    std::mutex mutex;
    std::binary_semaphore done{ 0 };
    std::atomic<bool> ready{ false };
    bool completed = false;
    bool cancelled = false;
    std::optional<R> result;
    // Continuations receive `&result` on success, `nullptr` on cancellation (so they
    // propagate cancellation to their own subsequent).
    std::vector<std::move_only_function<void(R*)>> continuations;

    template<typename Fn, typename Arg>
    void run(Fn& fn, Arg& arg)
    {
        complete(fn(arg));
    }

    void complete(R value)
    {
        std::vector<std::move_only_function<void(R*)>> conts;
        {
            std::scoped_lock lock(mutex);
            if (completed)   // idempotent: first settle wins
                return;
            result.emplace(std::move(value));
            completed = true;
            conts = std::move(continuations);
        }
        ready.store(true, std::memory_order_release);
        done.release();
        for (auto& c : conts)
            c(&*result);
    }

    void cancel()
    {
        std::vector<std::move_only_function<void(R*)>> conts;
        {
            std::scoped_lock lock(mutex);
            if (completed)
                return;
            completed = true;
            cancelled = true;
            conts = std::move(continuations);
        }
        ready.store(true, std::memory_order_release);
        done.release();
        for (auto& c : conts)
            c(nullptr);
    }

    void attach(std::move_only_function<void(R*)> cont)
    {
        {
            std::scoped_lock lock(mutex);
            if (!completed)
            {
                continuations.push_back(std::move(cont));
                return;
            }
        }
        cont(cancelled ? nullptr : &*result);
    }

    R get()
    {
        done.acquire();
        if (cancelled)
            ts::fatal("Task::get() on a cancelled task; check is_cancelled() first");
        return std::move(*result);
    }
};

template<>
struct Task_control_block<void>
{
    std::mutex mutex;
    std::condition_variable done_cv;   // wakes N waiters (a `Signal` is a barrier)
    std::atomic<bool> ready{ false };
    bool completed = false;
    bool cancelled = false;
    // Continuations receive the cancellation flag so they propagate it downstream.
    std::vector<std::move_only_function<void(bool)>> continuations;

    template<typename Fn, typename Arg>
    void run(Fn& fn, Arg& arg)
    {
        fn(arg);
        complete();
    }

    void complete() { settle(false); }
    void cancel()   { settle(true); }

    void settle(bool cancel_)
    {
        std::vector<std::move_only_function<void(bool)>> conts;
        {
            std::scoped_lock lock(mutex);
            if (completed)   // idempotent: first settle wins (Signal::trigger)
                return;
            completed = true;
            cancelled = cancel_;
            conts = std::move(continuations);
        }
        ready.store(true, std::memory_order_release);
        done_cv.notify_all();   // `completed` was set under the lock above: no lost wakeup
        for (auto& c : conts)
            c(cancelled);
    }

    void attach(std::move_only_function<void(bool)> cont)
    {
        {
            std::scoped_lock lock(mutex);
            if (!completed)
            {
                continuations.push_back(std::move(cont));
                return;
            }
        }
        cont(cancelled);
    }

    // Blocks until settled. Unlike the value overload, supports any number of
    // concurrent waiters (a `Signal` fans out to many). Cancellation is queried via
    // `Task::is_cancelled()`; a cancelled `void` get() simply returns.
    void get()
    {
        std::unique_lock lock(mutex);
        done_cv.wait(lock, [this] { return completed; });
    }
};

// --- apply-style continuation detection -----------------------------------
//
// A `then` off a tuple-valued task may take the tuple by reference (`then([](tuple&
// t){...})`) or, apply-style, its elements unpacked (`then([](A& a, B& b){...})`).

template<typename> inline constexpr bool is_tuple_v = false;
template<typename... Ts> inline constexpr bool is_tuple_v<std::tuple<Ts...>> = true;

template<typename Fn, typename Tuple> struct Apply_invocable : std::false_type {};
template<typename Fn, typename... Ts>
struct Apply_invocable<Fn, std::tuple<Ts...>> : std::bool_constant<std::is_invocable_v<Fn, Ts&...>> {};

// `then(fn)` unpacks when the producer is a tuple, `fn` does NOT take the tuple by
// reference, but it IS invocable with the tuple's elements.
template<typename Fn, typename R>
inline constexpr bool use_apply_v =
    is_tuple_v<R> && !std::is_invocable_v<Fn, R&> && Apply_invocable<Fn, R>::value;

template<typename Fn, typename Tuple>
using Apply_result_t = decltype(std::apply(std::declval<Fn&>(), std::declval<Tuple&>()));

} // namespace detail

// Handle to an async result. `get()` blocks for the result (call once); `then()`
// chains a continuation that runs when this task completes.
template<typename R>
class Task
{
public:
    Task() = default;

    explicit Task(std::shared_ptr<detail::Task_control_block<R>> state) noexcept
        : state_(std::move(state))
    {}

    bool is_done() const noexcept
    {
        return state_ && state_->ready.load(std::memory_order_acquire);
    }

    // True once the task has settled as cancelled (its body was skipped, or an
    // upstream cancellation propagated to it).
    bool is_cancelled() const noexcept
    {
        return state_ && state_->ready.load(std::memory_order_acquire) && state_->cancelled;
    }

    // Blocks until the task settles and returns its result. Call once. Fatal if the
    // task was cancelled (no result) — check `is_cancelled()` first.
    R get()
    {
        return state_->get();
    }

    // Chains a continuation. Runs `fn` when this task completes; if this task is
    // cancelled (or `token` is cancelled when the continuation fires), `fn` is skipped
    // and the cancellation propagates to the returned task. For a non-void producer
    // `fn` receives the result by reference (or, for a tuple, apply-style); for void
    // it takes no argument.
    template<typename Fn>
    auto then(Fn&& fn, Cancellation_token token = {})
    {
        if constexpr (std::is_void_v<R>)
        {
            using R2 = std::invoke_result_t<Fn>;
            auto next = std::make_shared<detail::Task_control_block<R2>>();
            state_->attach([next, fn = std::forward<Fn>(fn), token](bool cancelled) mutable
            {
                if (cancelled || token.is_cancel_requested())
                    next->cancel();
                else if constexpr (std::is_void_v<R2>) { fn(); next->complete(); }
                else next->complete(fn());
            });
            return Task<R2>(std::move(next));
        }
        else if constexpr (detail::use_apply_v<Fn, R>)
        {
            // Apply-style: unpack the tuple result into `fn`'s arguments.
            using R2 = detail::Apply_result_t<Fn, R>;
            auto next = std::make_shared<detail::Task_control_block<R2>>();
            state_->attach([next, fn = std::forward<Fn>(fn), token](R* r) mutable
            {
                if (!r || token.is_cancel_requested())
                    next->cancel();
                else if constexpr (std::is_void_v<R2>) { std::apply(fn, *r); next->complete(); }
                else next->complete(std::apply(fn, *r));
            });
            return Task<R2>(std::move(next));
        }
        else
        {
            using R2 = std::invoke_result_t<Fn, R&>;
            auto next = std::make_shared<detail::Task_control_block<R2>>();
            state_->attach([next, fn = std::forward<Fn>(fn), token](R* r) mutable
            {
                if (!r || token.is_cancel_requested())
                    next->cancel();
                else if constexpr (std::is_void_v<R2>) { fn(*r); next->complete(); }
                else next->complete(fn(*r));
            });
            return Task<R2>(std::move(next));
        }
    }

protected:
    detail::Task_control_block<R>* control() const noexcept { return state_.get(); }

private:
    std::shared_ptr<detail::Task_control_block<R>> state_;
};

namespace detail
{

// The tuple of the non-void prerequisite results (voids drop out).
template<typename... Rs>
using Kept_tuple_t = decltype(std::tuple_cat(
    std::declval<std::conditional_t<std::is_void_v<Rs>, std::tuple<>, std::tuple<Rs>>>()...));

// `when_all` result: void when nothing is kept (all prerequisites void), else the
// kept tuple.
template<typename... Rs>
using When_all_result_t = std::conditional_t<
    std::tuple_size_v<Kept_tuple_t<Rs...>> == 0, void, Kept_tuple_t<Rs...>>;

template<typename> struct To_optionals;
template<typename... Ts> struct To_optionals<std::tuple<Ts...>>
{
    using type = std::tuple<std::optional<Ts>...>;
};

// Slot index of each prerequisite in the kept tuple (-1 for a void prerequisite).
template<typename... Rs>
constexpr std::array<int, sizeof...(Rs)> when_all_slots()
{
    std::array<bool, sizeof...(Rs)> is_void{ std::is_void_v<Rs>... };
    std::array<int, sizeof...(Rs)> slot{};
    int next = 0;
    for (std::size_t i = 0; i < sizeof...(Rs); ++i)
        slot[i] = is_void[i] ? -1 : next++;
    return slot;
}

template<typename Slots, typename R_result>
void when_all_finish(const std::shared_ptr<Slots>& slots,
                     const std::shared_ptr<Task_control_block<R_result>>& next)
{
    if constexpr (std::is_void_v<R_result>)
        next->complete();
    else
        [&]<std::size_t... J>(std::index_sequence<J...>)
        {
            next->complete(R_result(std::move(*std::get<J>(*slots))...));   // move (supports move-only)
        }(std::make_index_sequence<std::tuple_size_v<Slots>>{});
}

template<int Slot, typename R, typename Slots, typename R_result>
void when_all_attach_one(Task<R> prereq,
                         std::shared_ptr<Slots> slots,
                         std::shared_ptr<std::atomic<int>> remaining,
                         std::shared_ptr<Task_control_block<R_result>> next)
{
    if constexpr (std::is_void_v<R>)
        prereq.then([slots, remaining, next]
        {
            if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1)
                when_all_finish(slots, next);
        });
    else
        prereq.then([slots, remaining, next](R& value)
        {
            std::get<Slot>(*slots).emplace(std::move(value));   // move out of the prerequisite
            if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1)
                when_all_finish(slots, next);
        });
}

} // namespace detail

// Typed join: completes when every prerequisite completes. Void prerequisites act as
// pure ordering (they drop out of the result); the results of the non-void ones are
// carried as a tuple (as `void` if all prerequisites are void). Results may be
// move-only. Consume with `.then` — either the tuple, or, apply-style, its elements
// unpacked (`then([](A& a, B& b){ ... })`).
template<typename... Rs>
Task<detail::When_all_result_t<Rs...>> when_all(Task<Rs>... prerequisites)
{
    static_assert(sizeof...(Rs) > 0, "when_all needs at least one task");

    using Result = detail::When_all_result_t<Rs...>;
    using Slots = typename detail::To_optionals<detail::Kept_tuple_t<Rs...>>::type;

    auto slots = std::make_shared<Slots>();
    auto remaining = std::make_shared<std::atomic<int>>(static_cast<int>(sizeof...(Rs)));
    auto next = std::make_shared<detail::Task_control_block<Result>>();

    constexpr auto slot = detail::when_all_slots<Rs...>();
    auto prereqs = std::make_tuple(std::move(prerequisites)...);

    [&]<std::size_t... I>(std::index_sequence<I...>)
    {
        (detail::when_all_attach_one<slot[I]>(
             std::get<I>(std::move(prereqs)), slots, remaining, next), ...);
    }(std::index_sequence_for<Rs...>{});

    return Task<Result>(next);
}

// A manually-completed synchronization point: a bodyless `Task<void>` (no work is
// scheduled or executed) that you `trigger()` by hand. It is both producer and
// consumer in one handle — the consumer side is inherited from `Task<void>`
// (`get`/`wait`, `is_done`, `then`), the producer side is `trigger()`. Copyable;
// copies share one control block. Used as a done-signal, a barrier / pipeline-phase
// gate, or an inter-task signal (the integrated equivalent of a manual-reset event
// / a promise+future fused). `trigger()` is idempotent (first call wins), so it is
// safe to trigger from multiple threads or more than once.
class Signal : public Task<void>
{
public:
    Signal()
        : Task<void>(std::make_shared<detail::Task_control_block<void>>())
    {}

    void trigger()
    {
        control()->complete();
    }
};

} // namespace ts
