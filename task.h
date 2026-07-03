#pragma once

#include "access.h"   // grant inheritance for launched/nested sub-work (snapshot_access)
#include "fatal.h"
#include "priority.h"

#include <array>
#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
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
// A default-constructed token is never cancelled. For a *push* notification (wake work
// that blocks rather than polls) register a `Cancel_callback` on the token.
class Cancel_callback;

namespace detail
{

// Shared cancellation state behind a source / its tokens / its callbacks. The request
// flag is atomic so the hot `is_cancel_requested()` needs no lock; the callback list
// (fired by `request_cancel`) is mutex-guarded, with `firing`/`firing_thread`/`done` for
// the teardown race (a `Cancel_callback` destroyed while it is being invoked).
struct Cancel_state
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

    explicit Cancellation_token(std::shared_ptr<detail::Cancel_state> state) noexcept
        : state_(std::move(state))
    {}

    std::shared_ptr<detail::Cancel_state> state_;
};

class Cancellation_source
{
public:
    Cancellation_source()
        : state_(std::make_shared<detail::Cancel_state>())
    {}

    // Request cancellation and fire every registered `Cancel_callback` synchronously, on
    // this thread. Idempotent — the first call wins; later calls (and callbacks registered
    // after) are no-ops / fire immediately.
    void request_cancel();

    bool is_cancel_requested() const noexcept { return state_->requested.load(std::memory_order_acquire); }
    Cancellation_token token() const noexcept { return Cancellation_token(state_); }

private:
    std::shared_ptr<detail::Cancel_state> state_;
};

// RAII push notification: registers `fn` on `token`; `request_cancel()` invokes it. If
// cancellation was already requested at construction, `fn` runs now, in the constructor.
// The destructor deregisters — and if the callback is mid-invocation on another thread it
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
            if (*it == this)
            {
                state_->callbacks.erase(it);   // not yet fired -> just deregister
                return;
            }
        if (state_->firing == this)            // mid-invocation
        {
            if (state_->firing_thread == std::this_thread::get_id())
                return;   // re-entrant self-destroy: detach (waiting would deadlock)
            state_->done.wait(lock, [&] { return state_->firing != this; });
        }
    }

    Cancel_callback(const Cancel_callback&) = delete;
    Cancel_callback& operator=(const Cancel_callback&) = delete;

private:
    friend class Cancellation_source;

    std::shared_ptr<detail::Cancel_state> state_;
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
    void (*execute)(const std::shared_ptr<Task_control_block>&, std::uint64_t generation) = nullptr;   // run the body (null => bodyless)
    // Fired once at `settle` (completed OR cancelled), after continuations/successors.
    // Unlike a continuation it is NOT consumed, so a reused block (e.g. a re-armed graph
    // node) keeps it across runs -- an alloc-free completion hook. Null for most tasks.
    void (*on_complete)(Task_control_block*) = nullptr;
    Cancellation_token token;          // checked by `execute` before running the body
    // Static dispatch properties, packed into one byte (all set at creation, never
    // mutated once the block is shared, so non-atomic is race-free). `priority` and
    // `run_inline` are read together at dispatch; `retractable` in the retraction guard.
    struct Flags
    {
        Priority priority : 2 = Priority::normal;   // queue position when dispatched
        bool retractable : 1 = false;               // safe to run inline from a waiter (no pipe/access binding)
        bool run_inline : 1 = false;                // dispatch on the settling thread, not the queue
    };
    Flags flags;
    // `num_locks`: below `execution_flag` it counts unmet PREREQUISITES (gate
    // execution); once the body starts the flag is set and it counts pending NESTED
    // tasks (gate completion). See docs/task-internals.md §4/§7.
    static constexpr std::uint32_t execution_flag = 0x8000'0000u;
    std::atomic<std::uint32_t> num_locks{ 0 };
    // One-runner claim + reuse generation fused into ONE atomic, so a stale dispatch and
    // a concurrent `reset()` can't be observed out of order (two separate atomics could,
    // letting a stale dispatch see the old generation but the new unclaimed state and
    // wrongly run). Bits [63:1] = generation (bumped by `reset`), bit [0] = body claimed.
    // A dispatch captures the generation it was queued for and calls `claim(gen)`: exactly
    // one caller (worker or retractor) wins per generation; a dispatch left stale by a
    // `reset` (retraction queues a duplicate the reset would otherwise let re-run the
    // body) fails the CAS and skips.
    std::atomic<std::uint64_t> run_state{ 0 };

    // This block's current reuse generation — the high bits of `run_state`, above the
    // claim bit (`run_state >> 1`). Bumped by `reset()` on each reuse. A dispatch captures
    // this value and passes it to `claim(gen)`; if `reset` bumped it in the meantime, that
    // dispatch is stale (a leftover from the prior run) and its claim CAS fails. See
    // `run_state` / `claim`.
    std::uint64_t generation() const noexcept { return run_state.load(std::memory_order_relaxed) >> 1; }

    // Claim the body for `gen`; true if this caller should run it. Fails if already
    // claimed (another runner) or if `gen` is no longer current (re-armed by `reset`).
    bool claim(std::uint64_t gen) noexcept
    {
        std::uint64_t expected = gen << 1;
        return run_state.compare_exchange_strong(
            expected, (gen << 1) | 1u, std::memory_order_acq_rel, std::memory_order_relaxed);
    }
    std::vector<std::shared_ptr<Task_control_block>> successors;      // decremented on settle
    std::vector<std::shared_ptr<Task_control_block>> prerequisites;   // backward links, for deep retraction
    std::vector<std::move_only_function<void(void*, bool)>> continuations;

    void complete() { settle(false); }
    void cancel()   { settle(true); }

    // A prerequisite or nested task settled (`after`/nesting are ordering-only, so a
    // cancelled one still releases). Decrement `blk`'s lock count and, when the last
    // lock drops, dispatch it (prerequisites met) or complete it (nested done).
    static void release(const std::shared_ptr<Task_control_block>& blk)
    {
        std::uint32_t now = blk->num_locks.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (now == 0)
            dispatch_ready(blk);          // pre-execution: prerequisites met -> queue, or run inline
        else if (now == execution_flag)
            blk->complete();              // post-execution: nested done -> complete
    }

    // Per-thread FIFO trampoline for inline tasks: a ready inline task runs on THIS thread
    // (the one that settled its last prerequisite), driven iteratively so a chain of inline
    // tasks doesn't recurse (settle -> release -> execute -> settle -> ...) and blow the
    // stack. The first inline dispatch on a thread starts the drain; inline tasks made
    // ready during the drain just push and are picked up in order (head advances as the
    // vector grows). `clear()` at the end retains capacity -> no steady-state allocation.
    inline static thread_local std::vector<std::shared_ptr<Task_control_block>> inline_pending;
    inline static thread_local bool inline_draining = false;

    static void dispatch_ready(const std::shared_ptr<Task_control_block>& blk)
    {
        if (!blk->flags.run_inline)
        {
            submit_ready(blk);   // queued: the scheduler runs it (at blk->flags.priority)
            return;
        }
        inline_pending.push_back(blk);
        if (inline_draining)
            return;              // an active drain on this thread will run it
        inline_draining = true;
        for (std::size_t head = 0; head < inline_pending.size(); ++head)
        {
            std::shared_ptr<Task_control_block> b = std::move(inline_pending[head]);
            if (b->execute)
                b->execute(b, b->generation());   // claims + runs the body on this thread
            else
                b->complete();
        }
        inline_pending.clear();   // retains capacity
        inline_draining = false;
    }

    void settle(bool cancel_)
    {
        std::vector<std::move_only_function<void(void*, bool)>> conts;
        std::vector<std::shared_ptr<Task_control_block>> succs;
        std::vector<std::shared_ptr<Task_control_block>> prereqs;   // drop (no longer needed)
        {
            std::scoped_lock lock(mutex);
            if (completed)
                return;
            completed = true;
            cancelled = cancel_;
            conts = std::move(continuations);
            succs = std::move(successors);
            prereqs = std::move(prerequisites);
        }
        ready.store(true, std::memory_order_release);
        done_cv.notify_all();   // `completed` set under the lock above: no lost wakeup
        void* r = cancel_ ? nullptr : result_ptr;
        for (auto& c : conts)
            c(r, cancel_);
        for (auto& s : succs)
            release(s);
        if (on_complete)
            on_complete(this);
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

    // Re-arm this settled block for another run (reuse — see `Task_builder::reset` /
    // `Signal::reset`). `successors`/`prerequisites`/`continuations` were drained by
    // `settle`, and the result storage is overwritten by the next run's body, so only
    // the completion scalars reset here. Leaves `num_locks` at 0 (the caller re-applies
    // any launch lock). Precondition: settled and quiescent — one run in flight, prior
    // result consumed; the `ready` gate rejects re-arming a task that has not settled.
    void reset()
    {
        if (!ready.load(std::memory_order_acquire))
            ts::fatal("Task_control_block::reset() on a task that has not settled");
        run_state.store((generation() + 1) << 1, std::memory_order_relaxed);   // next generation, unclaimed
        completed = false;
        cancelled = false;
        num_locks.store(0, std::memory_order_relaxed);
        ready.store(false, std::memory_order_release);
    }

    // Deep retraction: run the un-started part of `blk`'s dependency subtree inline on
    // the *calling* thread instead of parking on it — so a waiter under worker
    // exhaustion (nested fork-join) makes progress rather than deadlocking. Retract
    // `blk`'s prerequisites first (recursively), then, once its prerequisites are met
    // and it hasn't started, run its body inline. `execute` claims via `run_state`, so a
    // worker and a retractor never both run a body; non-retractable prerequisites
    // (pipe tasks, externally-triggered `Signal`s) are left to complete on their own.
    static void retract(const std::shared_ptr<Task_control_block>& blk)
    {
        if (!blk->flags.retractable || blk->ready.load(std::memory_order_acquire))
            return;

        std::vector<std::shared_ptr<Task_control_block>> prereqs;
        {
            std::scoped_lock lock(blk->mutex);
            prereqs = blk->prerequisites;   // snapshot (they clear as they settle)
        }
        for (const auto& p : prereqs)
            retract(p);

        if (blk->execute && blk->num_locks.load(std::memory_order_acquire) == 0)
            blk->execute(blk, blk->generation());   // ready & not started -> run inline (no-op if a worker beat us)
    }

    static void retract_or_wait(const std::shared_ptr<Task_control_block>& blk)
    {
        retract(blk);
        blk->wait();
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

    static void run(const std::shared_ptr<Task_control_block>& c, std::uint64_t gen)
    {
        if (!c->claim(gen))
            return;   // claimed by another runner, or stale after a reset

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

// Wrap `fn` so the task runs under the access grant active where it was BUILT (see
// access.h): sub-work launched from a task body inherits the launcher's permissions and
// may touch the launcher's guarded data. The snapshot is by value, so it is valid after
// the launcher unwinds and on whatever worker (or retractor) runs the body; a top-level
// build (no active grant) captures nothing, so the scope is a no-op. Shared by
// `ts::launch` and `ts::task` so both the eager and the builder path inherit alike.
template<typename R, typename Fn>
auto with_inherited_access(Fn&& fn)
{
    return [fn = std::forward<Fn>(fn), ctx = snapshot_access()]() mutable -> R
    {
        Inherited_access_scope scope(ctx);
        return fn();
    };
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

// Link `prereq` into `dependent`'s prerequisites as a RETRACTION HINT only — no lock
// count, no successor, completion stays whatever drives `dependent` (a continuation
// callback for `then`/`when_all`). It lets a blocking `get()` on `dependent` walk to
// `prereq` and run it inline (deep retraction) when `prereq` is retractable and
// un-started, instead of parking a worker. Skipped if `prereq` already settled (nothing
// to retract; its callback has fired or will fire). Pushed under `prereq`'s mutex like
// `add_prerequisite`, and only before `dependent` is exposed, so it doesn't race
// `dependent`'s later settle/retract.
inline void add_retraction_hint(const std::shared_ptr<Task_control_block>& prereq,
                                const std::shared_ptr<Task_control_block>& dependent)
{
    std::scoped_lock lock(prereq->mutex);
    if (!prereq->completed)
        dependent->prerequisites.push_back(prereq);
}

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
        detail::Task_control_block::retract_or_wait(core_);
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
            detail::add_retraction_hint(core_, next);   // deep-retractable: get() can run the producer inline
            next->flags.retractable = true;
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
            detail::add_retraction_hint(core_, next);   // deep-retractable: get() can run the producer inline
            next->flags.retractable = true;
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
        succ->prerequisites.push_back(prereq);   // backward link, for deep retraction
    }
}

} // namespace detail

// A configured-but-not-launched task: attach prerequisites, then `launch()`. Built by
// `ts::task(fn)`; owns the executable block until launched. `launch()` removes the
// "not launched" lock, so the task runs once every prerequisite has settled.
//
// The builder is also the **reusable** handle: it retains the block after `launch()`
// (which hands out a `Task<R>` aliasing the same block, but the builder keeps its own
// reference), so a body + result storage allocated once can be re-run many times. The
// pattern is `t.reset().after(...).launch(); r = t.get();` — `reset()` re-arms the block
// and the "not launched" lock for another run, prerequisites are re-established each run.
// One run in flight; `reset()` only after the prior run settled and its result was
// consumed. This avoids reallocation (pooling doesn't help — reuse is the point).
template<typename R>
class Task_builder
{
public:
    // Run after each prerequisite settles. Call before `launch()` (each run).
    template<typename... Ps>
    Task_builder& after(const Task<Ps>&... prerequisites)
    {
        (detail::add_prerequisite(detail::core_of(prerequisites), core_), ...);
        return *this;
    }

    // Set the queue priority for the run (applied when the task dispatches). Call before
    // `launch()`.
    Task_builder& priority(Priority p)
    {
        core_->flags.priority = p;
        return *this;
    }

    // Dispatch this task INLINE — run it on the thread that settles its last prerequisite,
    // rather than queueing it. For latency-sensitive / very small dependents. Trade-offs:
    // it runs on a nondeterministic thread (possibly external — see docs), bypasses
    // priority, and must not block; a deep inline chain is bounded by a trampoline. Call
    // before `launch()`.
    Task_builder& set_inline()
    {
        core_->flags.run_inline = true;
        return *this;
    }

    Task<R> launch(Cancellation_token token = {})
    {
        core_->token = std::move(token);
        detail::Task_control_block::release(core_);   // remove the launch lock
        return Task<R>(core_);
    }

    // Re-arm for another run: re-arm the block and restore the "not launched" lock, so
    // `after(...).launch()` runs it again. Precondition: the prior run settled and its
    // result was consumed (one run in flight). Chain: `t.reset().after(x).launch()`.
    Task_builder& reset()
    {
        core_->reset();
        core_->num_locks.store(1, std::memory_order_relaxed);   // the "not launched" lock
        return *this;
    }

    // Consume this run's result (see `Task<R>::get`) / query completion. The builder is
    // the handle; equivalently `launch()`'s returned `Task<R>` can be used.
    R get() { return Task<R>(core_).get(); }
    bool is_done() const noexcept { return Task<R>(core_).is_done(); }
    bool is_cancelled() const noexcept { return Task<R>(core_).is_cancelled(); }

private:
    template<typename Fn> friend auto task(Fn&& fn);

    explicit Task_builder(std::shared_ptr<detail::Task_control_block> core) noexcept
        : core_(std::move(core))
    {}

    std::shared_ptr<detail::Task_control_block> core_;
};

// Configure a standalone task (body + prerequisites) to launch later. `fn` takes no
// arguments (a bare scheduler task). Runs once all prerequisites (added via
// `.after(...)`) have settled. Inherits the launcher's access grant (like `ts::launch`),
// so a builder task used as nested sub-work may touch the parent's guarded data.
template<typename Fn>
auto task(Fn&& fn)
{
    using R = std::invoke_result_t<Fn>;
    auto core = detail::make_executable<R>(detail::with_inherited_access<R>(std::forward<Fn>(fn)), {});
    core->num_locks.store(1, std::memory_order_relaxed);   // the "not launched" lock
    core->flags.retractable = true;   // bare scheduler task: safe to run inline from a waiter
    return Task_builder<R>(std::move(core));
}

// Launch a standalone task on the scheduler — a bare functor with no access target (the
// primitive `async` for work that touches no guarded object). Returns a `Task<R>`; pass a
// `Cancellation_token` to make it skippable before it runs, and a `Priority` for its queue
// position. Dispatches through the `submit_ready` bridge (so this stays scheduler-
// independent) and inherits the launcher's access grant, so sub-work launched from a task
// body may touch the launcher's data.
template<typename Fn>
auto launch(Fn&& fn, Cancellation_token token = {}, Priority priority = Priority::normal)
    -> Task<std::invoke_result_t<Fn>>
{
    using R = std::invoke_result_t<Fn>;
    auto core = detail::make_executable<R>(detail::with_inherited_access<R>(std::forward<Fn>(fn)), std::move(token));
    core->flags.retractable = true;   // bare scheduler task (no pipe binding): safe to run inline from a waiter
    core->flags.priority = priority;
    detail::submit_ready(core);
    return Task<R>(core);
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

// Launch a task and attach it as a nested task of the currently-executing task (its
// completion gates the parent's). Sugar for `launch` + `add_nested`; call from within a
// task body.
template<typename Fn>
auto nested(Fn&& fn, Cancellation_token token = {}, Priority priority = Priority::normal)
    -> Task<std::invoke_result_t<Fn>>
{
    auto t = launch(std::forward<Fn>(fn), std::move(token), priority);
    add_nested(t);
    return t;
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
                         const std::shared_ptr<Task_control_block>& next_core,
                         std::shared_ptr<Slots> slots,
                         std::shared_ptr<std::atomic<int>> remaining,
                         std::shared_ptr<std::atomic<bool>> any_cancelled,
                         std::shared_ptr<std::function<void()>> finish)
{
    // Attach directly to the prerequisite (NOT via `.then`, which skips its continuation
    // on cancellation and would leave the join's `remaining` counter stuck above 0 -- the
    // join would never settle). On completion, store the result; on cancellation, flag the
    // join cancelled; either way decrement, and the last prerequisite to settle runs
    // `finish` (which completes or cancels the join accordingly).
    std::shared_ptr<Task_control_block> prereq_core = core_of(prereq);
    prereq_core->attach(
        [slots, remaining, any_cancelled, finish](void* r, bool cancelled)
        {
            if (cancelled)
                any_cancelled->store(true, std::memory_order_relaxed);
            else if constexpr (!std::is_void_v<R>)
                std::get<Slot>(*slots).emplace(std::move(*static_cast<R*>(r)));   // move out of the prerequisite
            if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1)
                (*finish)();
        });
    add_retraction_hint(prereq_core, next_core);   // deep-retractable: get() can run each prereq inline
}

} // namespace detail

// Typed join: completes when every prerequisite completes. Void prerequisites act as
// pure ordering (they drop out of the result); the results of the non-void ones are
// carried as a tuple (as `void` if all prerequisites are void). Results may be
// move-only. Consume with `.then` — either the tuple, or, apply-style, its elements
// unpacked (`then([](A& a, B& b){ ... })`). If any prerequisite is cancelled the join
// settles **cancelled** (it cannot form a complete tuple) rather than stalling; query
// with `Task::is_cancelled()`, and a `.then` off it propagates the cancellation.
template<typename... Rs>
Task<detail::When_all_result_t<Rs...>> when_all(Task<Rs>... prerequisites)
{
    static_assert(sizeof...(Rs) > 0, "when_all needs at least one task");

    using Result = detail::When_all_result_t<Rs...>;
    using Slots = typename detail::To_optionals<detail::Kept_tuple_t<Rs...>>::type;

    auto slots = std::make_shared<Slots>();
    auto remaining = std::make_shared<std::atomic<int>>(static_cast<int>(sizeof...(Rs)));
    // Set if any prerequisite settles cancelled: the join then cancels (it cannot form a
    // complete tuple), rather than stalling. `Task::is_cancelled()` reports it downstream.
    auto any_cancelled = std::make_shared<std::atomic<bool>>(false);

    std::shared_ptr<detail::Task_control_block> next_core;
    auto finish = std::make_shared<std::function<void()>>();

    if constexpr (std::is_void_v<Result>)
    {
        next_core = std::make_shared<detail::Task_control_block>();
        *finish = [core = next_core, any_cancelled]
        {
            if (any_cancelled->load(std::memory_order_relaxed))
                core->cancel();
            else
                core->complete();
        };
    }
    else
    {
        auto [core, wrapper] = detail::make_block<Result>();
        next_core = core;
        *finish = [wrapper, slots, any_cancelled]
        {
            if (any_cancelled->load(std::memory_order_relaxed))
            {
                wrapper->core.cancel();   // some slots are empty (cancelled prereqs) -> no tuple
                return;
            }
            [&]<std::size_t... J>(std::index_sequence<J...>)
            {
                wrapper->store(Result(std::move(*std::get<J>(*slots))...));   // move (move-only ok)
            }(std::make_index_sequence<std::tuple_size_v<Slots>>{});
            wrapper->core.complete();
        };
    }

    next_core->flags.retractable = true;   // a blocking get() can retract the (retractable) prerequisites

    constexpr auto slot = detail::when_all_slots<Rs...>();
    auto prereqs = std::make_tuple(std::move(prerequisites)...);

    [&]<std::size_t... I>(std::index_sequence<I...>)
    {
        (detail::when_all_attach_one<slot[I]>(
             std::get<I>(std::move(prereqs)), next_core, slots, remaining, any_cancelled, finish), ...);
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

    // Re-arm so it can be triggered again — a reusable barrier / phase gate. Precondition:
    // previously triggered and all waiters released (one use in flight).
    void reset()
    {
        control()->reset();
    }
};

} // namespace ts
