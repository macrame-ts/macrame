#pragma once

#include "access.h"
#include "scheduler.h"

#include <array>
#include <atomic>
#include <concepts>
#include <condition_variable>
#include <deque>
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

// Ambient scheduler used by `async()` (v1: a process-wide default).
Scheduler& default_scheduler();

namespace detail
{

// Submit a closure to the scheduler (bridges to the raw func-ptr API).
void submit_closure(Scheduler& scheduler, std::move_only_function<void()> closure);

struct Job
{
    Access mode;
    std::move_only_function<void()> fn;
    bool reservation = false;   // if set, `fn` is an on-acquired callback; see `pipe_reserve`
};

// A per-object reader/writer pipe. Jobs are admitted in FIFO order; consecutive
// readers run concurrently (each is its own scheduler task), a writer runs alone
// (no readers, no other writer). Different objects have independent pipes and run
// in parallel. Non-blocking: callers never wait; admission is completion-driven.
struct Pipe
{
    std::mutex mutex;
    std::condition_variable idle;
    std::deque<Job> jobs;
    int active_readers = 0;
    bool writer_active = false;

    // Blocks until the pipe is fully drained and nothing is in flight.
    void wait_until_idle()
    {
        std::unique_lock lock(mutex);
        idle.wait(lock, [this]
        {
            return jobs.empty() && active_readers == 0 && !writer_active;
        });
    }
};

void pipe_enqueue(Scheduler& scheduler, Pipe& pipe, Access mode, std::move_only_function<void()> fn);

// Reserve a pipe for exclusive out-of-band use (a `Static_task_graph` run accesses
// its objects directly, bypassing the pipe, so it must hold the pipe to keep async
// jobs from racing that direct access). Behaves as an exclusive (writer) holder that
// does not auto-complete: async jobs queue behind it until `pipe_release`.
// Returns true if acquired synchronously (pipe was idle); false if deferred, in which
// case `on_acquired` runs once the pipe drains to the reservation.
bool pipe_reserve(Scheduler& scheduler, Pipe& pipe, std::move_only_function<void()> on_acquired);

// Release a reservation taken by `pipe_reserve`; admits queued jobs.
void pipe_release(Scheduler& scheduler, Pipe& pipe);

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
    std::optional<R> result;
    std::vector<std::move_only_function<void(R&)>> continuations;

    template<typename Fn, typename Arg>
    void run(Fn& fn, Arg& arg)
    {
        complete(fn(arg));
    }

    void complete(R value)
    {
        std::vector<std::move_only_function<void(R&)>> conts;
        {
            std::scoped_lock lock(mutex);
            if (completed)   // idempotent: first completion wins
                return;
            result.emplace(std::move(value));
            completed = true;
            conts = std::move(continuations);
        }
        ready.store(true, std::memory_order_release);
        done.release();
        for (auto& c : conts)
            c(*result);
    }

    void attach(std::move_only_function<void(R&)> cont)
    {
        {
            std::scoped_lock lock(mutex);
            if (!completed)
            {
                continuations.push_back(std::move(cont));
                return;
            }
        }
        cont(*result);
    }

    R get()
    {
        done.acquire();
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
    std::vector<std::move_only_function<void()>> continuations;

    template<typename Fn, typename Arg>
    void run(Fn& fn, Arg& arg)
    {
        fn(arg);
        complete();
    }

    void complete()
    {
        std::vector<std::move_only_function<void()>> conts;
        {
            std::scoped_lock lock(mutex);
            if (completed)   // idempotent: first completion wins (Signal::trigger)
                return;
            completed = true;
            conts = std::move(continuations);
        }
        ready.store(true, std::memory_order_release);
        done_cv.notify_all();   // `completed` was set under the lock above: no lost wakeup
        for (auto& c : conts)
            c();
    }

    void attach(std::move_only_function<void()> cont)
    {
        {
            std::scoped_lock lock(mutex);
            if (!completed)
            {
                continuations.push_back(std::move(cont));
                return;
            }
        }
        cont();
    }

    // Blocks until completion. Unlike the value overload, supports any number of
    // concurrent waiters (a `Signal` fans out to many).
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

    // Blocks until the task completes and returns its result. Call once.
    R get()
    {
        return state_->get();
    }

    // Chains a continuation. For a non-void producer the continuation receives
    // the result by reference; for void it takes no argument. Returns a `Task` for
    // the continuation's own result.
    template<typename Fn>
    auto then(Fn&& fn)
    {
        if constexpr (std::is_void_v<R>)
        {
            using R2 = std::invoke_result_t<Fn>;
            auto next = std::make_shared<detail::Task_control_block<R2>>();
            state_->attach([next, fn = std::forward<Fn>(fn)]() mutable
            {
                if constexpr (std::is_void_v<R2>)
                {
                    fn();
                    next->complete();
                }
                else
                {
                    next->complete(fn());
                }
            });
            return Task<R2>(std::move(next));
        }
        else if constexpr (detail::use_apply_v<Fn, R>)
        {
            // Apply-style: unpack the tuple result into `fn`'s arguments.
            using R2 = detail::Apply_result_t<Fn, R>;
            auto next = std::make_shared<detail::Task_control_block<R2>>();
            state_->attach([next, fn = std::forward<Fn>(fn)](R& r) mutable
            {
                if constexpr (std::is_void_v<R2>)
                {
                    std::apply(fn, r);
                    next->complete();
                }
                else
                {
                    next->complete(std::apply(fn, r));
                }
            });
            return Task<R2>(std::move(next));
        }
        else
        {
            using R2 = std::invoke_result_t<Fn, R&>;
            auto next = std::make_shared<detail::Task_control_block<R2>>();
            state_->attach([next, fn = std::forward<Fn>(fn)](R& r) mutable
            {
                if constexpr (std::is_void_v<R2>)
                {
                    fn(r);
                    next->complete();
                }
                else
                {
                    next->complete(fn(r));
                }
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

// The only sanctioned way to touch a `T` across threads. You never receive a bare
// `T&`; you hand a functor to `async()` and it runs once access has been granted.
// Access mode is deduced from the functor's parameter const-ness:
//   `functor(T&)`       -> `read_write`
//   `functor(const T&)` -> `read_only`
class Static_task_graph;

template<typename T>
class Thread_safe
{
    friend class Static_task_graph;

public:
    // Non-explicit default ctor (value-initializes `T`) so arrays of `Thread_safe`
    // work; the forwarding ctor below handles explicit argument construction.
    Thread_safe() requires std::default_initializable<T>
        : instance_()
    {}

    // Constrained so it never shadows the (deleted) copy/move constructors:
    // 1+ args, and only when T is actually constructible from them.
    template<typename... Args>
        requires (sizeof...(Args) >= 1) && std::constructible_from<T, Args...>
    explicit Thread_safe(Args&&... args)
        : instance_(std::forward<Args>(args)...)
    {}

    // Identity matters (it is the access key); waits out pending jobs so the
    // pipe outlives its last task.
    ~Thread_safe()
    {
        pipe_.wait_until_idle();
    }

    Thread_safe(const Thread_safe&) = delete;
    Thread_safe& operator=(const Thread_safe&) = delete;

    // `read_write`: functor takes `T&` (and not `const T&`)
    template<typename Fn>
        requires std::invocable<Fn, T&> && (!std::invocable<Fn, const T&>)
    auto async(Fn&& fn) -> Task<std::invoke_result_t<Fn, T&>>
    {
        return launch<std::invoke_result_t<Fn, T&>, Access::read_write>(
            &instance_, std::forward<Fn>(fn));
    }

    // `read_only`: functor takes `const T&`
    template<typename Fn>
        requires std::invocable<Fn, const T&>
    auto async(Fn&& fn) const -> Task<std::invoke_result_t<Fn, const T&>>
    {
        return launch<std::invoke_result_t<Fn, const T&>, Access::read_only>(
            &instance_, std::forward<Fn>(fn));
    }

private:
    template<typename R, Access mode, typename Inst, typename Fn>
    Task<R> launch(Inst* inst, Fn&& fn) const
    {
        auto state = std::make_shared<detail::Task_control_block<R>>();

        detail::pipe_enqueue(default_scheduler(), pipe_, mode,
            [inst, state, fn = std::forward<Fn>(fn)]() mutable
            {
                Access_context ctx;
                ctx.add(inst, mode);
                Access_scope scope(ctx);
                state->run(fn, *inst);
            });

        return Task<R>(std::move(state));
    }

    T instance_;
    mutable detail::Pipe pipe_;
};

} // namespace ts
