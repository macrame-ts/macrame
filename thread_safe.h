#pragma once

#include "access.h"
#include "scheduler.h"

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

// Back-reference a graph node carries so `Task::after`/`before` can add edges
// without `Task` knowing the concrete graph type.
struct Graph_node_ref
{
    void* graph = nullptr;
    int index = -1;
    void (*link)(void* graph, int prerequisite, int successor) = nullptr;
};

struct Job
{
    Access mode;
    std::move_only_function<void()> fn;
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

// --- Task completion state -------------------------------------------------
//
// Holds the result plus a list of continuations attached via `Task::then()`.
// Continuations fire when the producer completes (inline on the completing
// worker); a continuation attached after completion runs inline on the caller.
// Note: mixing `get()` and `then()` on the same task, or calling `get()` twice, is
// unsupported (`get()` moves the result out).

template<typename R>
struct Task_state
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
struct Task_state<void>
{
    std::mutex mutex;
    std::binary_semaphore done{ 0 };
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
            completed = true;
            conts = std::move(continuations);
        }
        ready.store(true, std::memory_order_release);
        done.release();
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

    void get()
    {
        done.acquire();
    }
};

} // namespace detail

// Handle to an async result. `get()` blocks for the result (call once); `then()`
// chains a continuation that runs when this task completes.
template<typename R>
class Task
{
public:
    Task() = default;

    explicit Task(std::shared_ptr<detail::Task_state<R>> state) noexcept
        : state_(std::move(state))
    {}

    // Graph-node handle: no result state, carries the edge back-reference.
    Task(std::shared_ptr<detail::Task_state<R>> state, detail::Graph_node_ref node_ref) noexcept
        : state_(std::move(state))
        , node_ref_(node_ref)
    {}

    bool is_done() const noexcept
    {
        return state_ && state_->ready.load(std::memory_order_acquire);
    }

    // Ordering edges for `Static_task_graph` nodes (no-op on dynamic tasks).
    template<typename R2>
    Task& after(const Task<R2>& prerequisite)
    {
        if (node_ref_.link && node_ref_.graph == prerequisite.node_ref_.graph)
            node_ref_.link(node_ref_.graph, prerequisite.node_ref_.index, node_ref_.index);
        return *this;
    }

    template<typename R2>
    Task& before(const Task<R2>& successor)
    {
        if (node_ref_.link && node_ref_.graph == successor.node_ref_.graph)
            node_ref_.link(node_ref_.graph, node_ref_.index, successor.node_ref_.index);
        return *this;
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
            auto next = std::make_shared<detail::Task_state<R2>>();
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
        else
        {
            using R2 = std::invoke_result_t<Fn, R&>;
            auto next = std::make_shared<detail::Task_state<R2>>();
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

private:
    template<typename> friend class Task;

    std::shared_ptr<detail::Task_state<R>> state_;
    detail::Graph_node_ref node_ref_;
};

namespace detail
{

template<typename... Rs>
void when_all_finish(const std::shared_ptr<std::tuple<std::optional<Rs>...>>& slots,
                     const std::shared_ptr<Task_state<std::tuple<Rs...>>>& next)
{
    [&]<std::size_t... J>(std::index_sequence<J...>)
    {
        next->complete(std::tuple<Rs...>(std::move(*std::get<J>(*slots))...));
    }(std::index_sequence_for<Rs...>{});
}

template<std::size_t... I, typename... Rs>
void when_all_attach(std::index_sequence<I...>,
                     std::shared_ptr<std::tuple<std::optional<Rs>...>> slots,
                     std::shared_ptr<std::atomic<int>> remaining,
                     std::shared_ptr<Task_state<std::tuple<Rs...>>> next,
                     Task<Rs>... prerequisites)
{
    (prerequisites.then([slots, remaining, next](Rs& value)
    {
        std::get<I>(*slots) = value;
        if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1)
            when_all_finish<Rs...>(slots, next);
    }), ...);
}

} // namespace detail

// Typed join: completes when every prerequisite completes, carrying their
// results as a tuple into the subsequent (consume with `.then`). Prerequisite
// result types must be non-void and copyable. (Apply-style unpacking of the
// tuple into separate continuation args is future work; see docs/TODO.md.)
template<typename... Rs>
Task<std::tuple<Rs...>> when_all(Task<Rs>... prerequisites)
{
    static_assert(sizeof...(Rs) > 0, "when_all needs at least one task");

    auto slots = std::make_shared<std::tuple<std::optional<Rs>...>>();
    auto remaining = std::make_shared<std::atomic<int>>(static_cast<int>(sizeof...(Rs)));
    auto next = std::make_shared<detail::Task_state<std::tuple<Rs...>>>();

    detail::when_all_attach(std::index_sequence_for<Rs...>{},
        slots, remaining, next, std::move(prerequisites)...);

    return Task<std::tuple<Rs...>>(next);
}

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
        auto state = std::make_shared<detail::Task_state<R>>();

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
