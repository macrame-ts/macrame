#pragma once

#include "fatal.h"

#include <array>
#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ts
{

template<typename R> class Task;

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

struct Task_control_block;

// Submit a block whose prerequisites are all met to run (defined in the scheduler
// layer; task.h stays scheduler-independent). Runs `execute` if it has a body, else
// `complete`s it.
void submit_ready(std::shared_ptr<Task_control_block> block);

// --- Task control block ----------------------------------------------------
//
// The refcounted completion/dependency core behind a `Task<R>` handle. FULLY
// MONOMORPHIC — parameterized on nothing. The result type is erased behind a
// `void* result_ptr` (nullptr => no result: `void`/bodyless); a body, when present,
// hangs off a `Result_block<R>`/executable wrapper that has `core` as its first
// member so a `Task_control_block*` aliases it (see docs/task-internals.md §2).
// Continuations receive `(result_ptr-or-nullptr, cancelled)` so they propagate a
// cancellation to their own subsequent. `settle()` is idempotent — the first settle
// wins (so a bodyless block can be triggered; see `Signal`). Note: mixing `get()` and
// `then()` on one task, or `get()` twice, is unsupported (`get()` moves the result).
struct Task_control_block
{
    std::mutex mutex;
    std::condition_variable done_cv;   // wakes N waiters (a `Signal` is a barrier)
    std::atomic<bool> ready{ false };
    bool completed = false;
    bool cancelled = false;
    void* result_ptr = nullptr;        // -> the wrapper's stored R (set before complete), or null
    void (*execute)(const std::shared_ptr<Task_control_block>&) = nullptr;   // run the body (null => bodyless)
    Cancellation_token token;          // checked by `execute` before running the body
    // `num_locks`: below `execution_flag` it counts unmet PREREQUISITES (gate
    // execution); once the body starts the flag is set and it counts pending NESTED
    // tasks (gate completion). See docs/task-internals.md §4/§7.
    static constexpr std::uint32_t execution_flag = 0x8000'0000u;
    std::atomic<std::uint32_t> num_locks{ 0 };
    std::vector<std::shared_ptr<Task_control_block>> successors;   // decremented on settle
    std::vector<std::move_only_function<void(void*, bool)>> continuations;

    void complete() { settle(false); }
    void cancel()   { settle(true); }

    // A prerequisite or nested task settled (`after`/nesting are ordering-only, so a
    // cancelled one still releases). Decrement `blk`'s lock count and, when the last
    // lock drops, schedule it (prerequisites met) or complete it (nested done).
    static void release(const std::shared_ptr<Task_control_block>& blk)
    {
        std::uint32_t now = blk->num_locks.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (now == 0)
            submit_ready(blk);            // pre-execution: prerequisites met -> schedule
        else if (now == execution_flag)
            blk->complete();              // post-execution: nested done -> complete
    }

    void settle(bool cancel_)
    {
        std::vector<std::move_only_function<void(void*, bool)>> conts;
        std::vector<std::shared_ptr<Task_control_block>> succs;
        {
            std::scoped_lock lock(mutex);
            if (completed)
                return;
            completed = true;
            cancelled = cancel_;
            conts = std::move(continuations);
            succs = std::move(successors);
        }
        ready.store(true, std::memory_order_release);
        done_cv.notify_all();   // `completed` set under the lock above: no lost wakeup
        void* r = cancel_ ? nullptr : result_ptr;
        for (auto& c : conts)
            c(r, cancel_);
        for (auto& s : succs)
            release(s);
    }

    void attach(std::move_only_function<void(void*, bool)> cont)
    {
        {
            std::scoped_lock lock(mutex);
            if (!completed)
            {
                continuations.push_back(std::move(cont));
                return;
            }
        }
        cont(cancelled ? nullptr : result_ptr, cancelled);
    }

    void wait()
    {
        std::unique_lock lock(mutex);
        done_cv.wait(lock, [this] { return completed; });
    }
};

// Result storage composed around the monomorphic block. `core` is the FIRST member so
// a `Task_control_block*` aliases the wrapper. `store()` moves the value in and points
// `result_ptr` at it (before the caller `complete()`s the core).
template<typename R>
struct Result_block
{
    Task_control_block core;
    std::optional<R> result;

    void store(R value)
    {
        result.emplace(std::move(value));
        core.result_ptr = &*result;
    }
};

// Make a fresh block for a `Task<R>`; returns the handle core plus (for a non-void R)
// the typed wrapper the producer stores into.
template<typename R>
auto make_block()
{
    if constexpr (std::is_void_v<R>)
    {
        return std::make_shared<Task_control_block>();
    }
    else
    {
        auto wrapper = std::make_shared<Result_block<R>>();
        std::shared_ptr<Task_control_block> core(wrapper, &wrapper->core);   // aliasing
        return std::pair{ std::move(core), std::move(wrapper) };
    }
}

// The task currently executing on this thread (for nested-task attachment). A
// shared_ptr so a nested child can register the parent as its successor and keep it
// alive until the child completes.
inline thread_local std::shared_ptr<Task_control_block> current_task;

// Empty for `void`, so an executable `void` task pays nothing for a result.
template<typename R> struct Result_storage { std::optional<R> result; };
template<> struct Result_storage<void> {};

// An executable task: the monomorphic block (FIRST member, so a `Task_control_block*`
// aliases / `reinterpret_cast`s back to it) + result storage + the body + a token.
// `run` is wired into `core.execute`; the scheduler/pipe invokes it via
// `block->execute(block)`. The body lives here (reachable from the block for future
// reuse/retraction); its type is erased behind the `execute` function pointer, so the
// block and everything downstream stay monomorphic.
template<typename Body, typename R>
struct Executable
{
    Task_control_block core;   // MUST be first
    Result_storage<R> storage;   // empty for void
    Body body;

    explicit Executable(Body b)
        : body(std::move(b))
    {}

    static void run(const std::shared_ptr<Task_control_block>& c)
    {
        auto* self = reinterpret_cast<Executable*>(c.get());
        if (c->token.is_cancel_requested())   // not-yet-started: skip body, settle cancelled
        {
            c->cancel();
            return;
        }

        // Switch the counter to completion-lock mode: the flag + a self-lock held for
        // the body. Nested tasks launched during the body add more locks.
        c->num_locks.store(Task_control_block::execution_flag + 1, std::memory_order_relaxed);
        auto prev = std::move(current_task);
        current_task = c;

        if constexpr (std::is_void_v<R>)
            self->body();
        else
        {
            self->storage.result.emplace(self->body());
            c->result_ptr = &*self->storage.result;
        }

        current_task = std::move(prev);

        // Drop the self-lock. If it was the only remaining lock, no nested tasks are
        // pending -> complete now; otherwise the last nested task will complete us.
        if (c->num_locks.fetch_sub(1, std::memory_order_acq_rel) == Task_control_block::execution_flag + 1)
            c->complete();
    }
};

// Build an executable task; returns the handle core with `execute` wired up. Submit
// `[core]{ core->execute(core.get()); }` (to the scheduler or a pipe) to run it.
template<typename R, typename Body>
std::shared_ptr<Task_control_block> make_executable(Body&& body, Cancellation_token token)
{
    using Exec = Executable<std::decay_t<Body>, R>;
    auto exec = std::make_shared<Exec>(std::forward<Body>(body));
    exec->core.execute = &Exec::run;
    exec->core.token = std::move(token);
    return std::shared_ptr<Task_control_block>(exec, &exec->core);   // aliasing
}

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

// The control block behind a `Task` handle (used to wire prerequisites).
template<typename R>
std::shared_ptr<Task_control_block> core_of(const Task<R>& t) noexcept;

} // namespace detail

// Handle to an async result. `get()` blocks for the result (call once); `then()`
// chains a continuation that runs when this task completes.
template<typename R>
class Task
{
public:
    Task() = default;

    explicit Task(std::shared_ptr<detail::Task_control_block> core) noexcept
        : core_(std::move(core))
    {}

    bool is_done() const noexcept
    {
        return core_ && core_->ready.load(std::memory_order_acquire);
    }

    // True once the task has settled as cancelled (its body was skipped, or an
    // upstream cancellation propagated to it).
    bool is_cancelled() const noexcept
    {
        return core_ && core_->ready.load(std::memory_order_acquire) && core_->cancelled;
    }

    // Blocks until the task settles and returns its result. Call once. For a value
    // task, fatal if it was cancelled (no result) — check `is_cancelled()` first;
    // a cancelled `void` get() simply returns.
    R get()
    {
        core_->wait();
        if constexpr (std::is_void_v<R>)
        {
            return;
        }
        else
        {
            if (core_->cancelled)
                ts::fatal("Task::get() on a cancelled task; check is_cancelled() first");
            return std::move(*static_cast<R*>(core_->result_ptr));
        }
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
            return chain<R2>([fn = std::forward<Fn>(fn)](void*) mutable -> R2
            {
                return fn();
            }, token);
        }
        else if constexpr (detail::use_apply_v<Fn, R>)
        {
            using R2 = detail::Apply_result_t<Fn, R>;
            return chain<R2>([fn = std::forward<Fn>(fn)](void* r) mutable -> R2
            {
                return std::apply(fn, *static_cast<R*>(r));
            }, token);
        }
        else
        {
            using R2 = std::invoke_result_t<Fn, R&>;
            return chain<R2>([fn = std::forward<Fn>(fn)](void* r) mutable -> R2
            {
                return fn(*static_cast<R*>(r));
            }, token);
        }
    }

protected:
    detail::Task_control_block* control() const noexcept { return core_.get(); }

private:
    template<typename R2>
    friend std::shared_ptr<detail::Task_control_block> detail::core_of(const Task<R2>&) noexcept;

    // Attach a continuation that produces R2 (via `produce(result_ptr)`) into a fresh
    // block, propagating cancellation. Returns the `Task<R2>` handle.
    template<typename R2, typename Produce>
    Task<R2> chain(Produce produce, Cancellation_token token)
    {
        if constexpr (std::is_void_v<R2>)
        {
            auto next = std::make_shared<detail::Task_control_block>();
            core_->attach([next, produce = std::move(produce), token](void* r, bool cancelled) mutable
            {
                if (cancelled || token.is_cancel_requested())
                    next->cancel();
                else { produce(r); next->complete(); }
            });
            return Task<R2>(std::move(next));
        }
        else
        {
            auto [next, wrapper] = detail::make_block<R2>();
            core_->attach([wrapper, produce = std::move(produce), token](void* r, bool cancelled) mutable
            {
                if (cancelled || token.is_cancel_requested())
                    wrapper->core.cancel();
                else { wrapper->store(produce(r)); wrapper->core.complete(); }
            });
            return Task<R2>(std::move(next));
        }
    }

    std::shared_ptr<detail::Task_control_block> core_;
};

namespace detail
{

template<typename R>
std::shared_ptr<Task_control_block> core_of(const Task<R>& t) noexcept { return t.core_; }

// Register `succ` as a successor of `prereq` (bumping its lock count), unless `prereq`
// has already settled. `after` is ordering-only — a cancelled prerequisite releases
// its successors just like a completed one.
inline void add_prerequisite(const std::shared_ptr<Task_control_block>& prereq,
                             const std::shared_ptr<Task_control_block>& succ)
{
    std::scoped_lock lock(prereq->mutex);
    if (!prereq->completed)
    {
        succ->num_locks.fetch_add(1, std::memory_order_relaxed);
        prereq->successors.push_back(succ);
    }
}

} // namespace detail

// A configured-but-not-launched task: attach prerequisites, then `launch()`. Built by
// `ts::task(fn)`; owns the executable block until launched. `launch()` removes the
// "not launched" lock, so the task runs once every prerequisite has settled.
template<typename R>
class Task_builder
{
public:
    // Run after each prerequisite settles. Call before `launch()`.
    template<typename... Ps>
    Task_builder& after(const Task<Ps>&... prerequisites)
    {
        (detail::add_prerequisite(detail::core_of(prerequisites), core_), ...);
        return *this;
    }

    Task<R> launch(Cancellation_token token = {})
    {
        core_->token = std::move(token);
        detail::Task_control_block::release(core_);   // remove the launch lock
        return Task<R>(core_);
    }

private:
    template<typename Fn> friend auto task(Fn&& fn);

    explicit Task_builder(std::shared_ptr<detail::Task_control_block> core) noexcept
        : core_(std::move(core))
    {}

    std::shared_ptr<detail::Task_control_block> core_;
};

// Configure a standalone task (body + prerequisites) to launch later. `fn` takes no
// arguments (a bare scheduler task). Runs once all prerequisites (added via
// `.after(...)`) have settled.
template<typename Fn>
auto task(Fn&& fn)
{
    using R = std::invoke_result_t<Fn>;
    auto core = detail::make_executable<R>(std::forward<Fn>(fn), {});
    core->num_locks.store(1, std::memory_order_relaxed);   // the "not launched" lock
    return Task_builder<R>(std::move(core));
}

// Attach `child` as a NESTED task of the currently-executing task: that task will not
// complete until `child` settles (completed or cancelled). Call from within a task
// body (async / launch / task); fatal if there is no running task. `child` can be
// launched any way — nesting is a completion dependency, orthogonal to how it runs.
template<typename R>
void add_nested(const Task<R>& child)
{
    std::shared_ptr<detail::Task_control_block> parent = detail::current_task;
    if (!parent)
        ts::fatal("add_nested called outside a running task");

    parent->num_locks.fetch_add(1, std::memory_order_relaxed);   // a completion lock on the parent

    std::shared_ptr<detail::Task_control_block> child_core = detail::core_of(child);
    {
        std::scoped_lock lock(child_core->mutex);
        if (!child_core->completed)
        {
            child_core->successors.push_back(std::move(parent));   // child releases parent when it settles
            return;
        }
    }
    detail::Task_control_block::release(parent);   // child already settled -> release the lock now
}

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

template<int Slot, typename R, typename Slots>
void when_all_attach_one(Task<R> prereq,
                         std::shared_ptr<Slots> slots,
                         std::shared_ptr<std::atomic<int>> remaining,
                         std::shared_ptr<std::function<void()>> finish)
{
    if constexpr (std::is_void_v<R>)
        prereq.then([remaining, finish]
        {
            if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1)
                (*finish)();
        });
    else
        prereq.then([slots, remaining, finish](R& value)
        {
            std::get<Slot>(*slots).emplace(std::move(value));   // move out of the prerequisite
            if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1)
                (*finish)();
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

    std::shared_ptr<detail::Task_control_block> next_core;
    auto finish = std::make_shared<std::function<void()>>();

    if constexpr (std::is_void_v<Result>)
    {
        next_core = std::make_shared<detail::Task_control_block>();
        *finish = [core = next_core] { core->complete(); };
    }
    else
    {
        auto [core, wrapper] = detail::make_block<Result>();
        next_core = core;
        *finish = [wrapper, slots]
        {
            [&]<std::size_t... J>(std::index_sequence<J...>)
            {
                wrapper->store(Result(std::move(*std::get<J>(*slots))...));   // move (move-only ok)
            }(std::make_index_sequence<std::tuple_size_v<Slots>>{});
            wrapper->core.complete();
        };
    }

    constexpr auto slot = detail::when_all_slots<Rs...>();
    auto prereqs = std::make_tuple(std::move(prerequisites)...);

    [&]<std::size_t... I>(std::index_sequence<I...>)
    {
        (detail::when_all_attach_one<slot[I]>(
             std::get<I>(std::move(prereqs)), slots, remaining, finish), ...);
    }(std::index_sequence_for<Rs...>{});

    return Task<Result>(std::move(next_core));
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
        : Task<void>(std::make_shared<detail::Task_control_block>())
    {}

    void trigger()
    {
        control()->complete();
    }
};

} // namespace ts
