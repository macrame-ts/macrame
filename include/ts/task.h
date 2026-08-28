#pragma once

// The task layer. `Task<R>` is the one completion handle behind every async result
// (`access`/`async`/`launch`/a graph run/a coroutine): `co_await` it inside a task,
// `sync()`/`take()` it from outside (the blue boundary - blocking inside a task is fatal).
// Also here: the option aggregates (`Dispatch_options`, `Access_options`), `ts::launch`
// (run a bare functor on the scheduler, detached - it inherits no access), `Signal` (a
// hand-triggered task: done-signal, barrier, the bridge for OS/GPU completions), and
// `External_wait` (declare an off-pool completion so the deadlock net does not misfire).
// Cooperative cancellation lives in ts/cancellation.h (included here, so it remains
// available through this header); the control block behind the handle in
// ts/detail/task_block.h. User guide: docs/guide.md §4; internals: docs/internals/task-internals.md.

#include "ts/cancellation.h"
#include "ts/fatal.h"
#include "ts/named.h"
#include "ts/priority.h"
#include "ts/rules.h"
#include "ts/detail/task_block.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <source_location>
#include <type_traits>
#include <utility>

namespace ts
{

template<typename R> class Task;

namespace detail
{

// The control block behind a `Task` handle (for detail-layer wiring).
template<typename R>
Task_ptr core_of(const Task<R>& t) noexcept;

// `core_of`'s inverse: wrap an existing block in a handle. For detail-layer producers
// (`Deferred::commit`'s pre-settled sentinel) that hand out a `Task` for a block they
// did not create through the public builders.
template<typename R>
Task<R> task_from_core(Task_ptr core) noexcept;

// What `Task<R>::as_optional()` returns: a marker carrying the block, made awaitable by an
// `operator co_await` in coroutine_support.h that resolves to `std::optional<R>` - empty
// when the task settled cancelled. Declared here so `Task` can name it without dragging the
// coroutine layer into this header.
template<typename R>
struct Optional_awaitable
{
    Task_ptr core;
};

} // namespace detail

// Options for every verb that always schedules its body: `ts::launch`, `Guarded::async`, the
// free `ts::async(fn, objs...)`, `Deferred::commit` and `Versioned::publish`. `token` makes the
// body skippable before it runs (and is forwarded to a trailing-`Cancellation_token` body for a
// mid-run early-out); `priority` sets its queue position.
struct Dispatch_options
{
    Cancellation_token token = {};
    std::optional<Priority> priority{};   // unset = inherit the calling task's (`detail::resolved_priority`)
    // Optional debug identity for the task: a literal (`{.name = "hud"}`) or a call site
    // (`{.name = ts::Named{}}`). Left empty, a verb that can capture one falls back to the site
    // its own defaulted `std::source_location` recorded. The multi-object verbs end in an
    // object pack, so no defaulted `source_location` is expressible after it and this field is
    // the only identity they can carry - which is why it is a `Named` and not a literal.
    Named name = Named(nullptr);
};

// Options for the opportunistic access verbs - `Guarded::access`, the free
// `ts::access(fn, objs...)`, `Versioned::read` and the `Access_op` constructors. The same three
// dispatch fields, plus the one knob only these verbs can honour: they pick inline-vs-enqueued
// per fire (lend what the calling task already holds, run inline when every remaining object is
// free, else enqueue), and `queued` is the opt-out. The split is what keeps every field live on
// every surface that takes it - a verb that always enqueues takes `Dispatch_options`, which has
// no knob to ignore.
//
// Not derived from `Dispatch_options`: a base subobject cannot be reached by a designator, so
// `{.priority = p}` would stop compiling.
struct Access_options
{
    Cancellation_token token = {};
    std::optional<Priority> priority{};   // as `Dispatch_options::priority`
    // As `Dispatch_options::name`.
    Named name = Named(nullptr);
    // Never run the body inline on the calling thread - the attended-but-never-inline
    // quadrant, for a heavy body whose result the caller still stays for. Skips only the
    // inline-when-free arm; an access whose objects the calling task already holds is lent
    // and runs inline regardless - lending is correctness, not opportunism: an access queued
    // behind its own caller's held grant deadlocks when awaited.
    bool queued = false;
};

// The library-wide `[[nodiscard]]` policy for handle-returning verbs, stated once here
// because the attribute is spread across `guarded.h`, `versioned.h`, `deferred.h`,
// `frame_gate.h`, `parallel_for.h` and `static_task_graph.h`:
//
//   marked   - a verb whose handle is the caller's only way to meet an obligation the
//              library checks later. Dropping it is not "fire and forget", it is a
//              deferred fatal: `Guarded::access` / `Versioned::read` (the op's destructor
//              blocks, and its diagnostic fires only when it actually has to wait, so a
//              discard trips a nondeterministic report), `Deferred::commit` and
//              `Versioned::publish` (destroying with the write still in flight is fatal),
//              `Static_task_graph::execute` (the run's only completion signal),
//              `Frame_gate::next` (a discarded gate parks nobody), and
//              `parallel_for_async` (nothing else joins the slices).
//   unmarked - a verb where not waiting is the point: `ts::launch`, `Guarded::async` and
//              the free `ts::async`. Detaching is sanctioned there, so an attribute would
//              only teach users to write `(void)`.
//
// `Task<R>` itself carries no attribute: it is the return type of both kinds.

// Handle to an async result. `co_await` it from a coroutine task (the sanctioned
// composition - see coroutine_support.h); `sync()` blocks for the result from a blue
// (non-task) thread.
template<typename R>
class Task
{
public:
    Task() = default;

    explicit Task(detail::Task_ptr core) noexcept
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

    // Blocks until the task settles and returns its result **by `const&`** (non-consuming):
    // any number of readers may `sync()` the same task (returns `const R&` for a value task,
    // `void` for a void one). The reference is valid while a handle (this `Task` or
    // another copy) keeps the block alive; `T r = t.sync()` copies within the
    // full-expression and is always safe. To *move* the result out (ownership handoff, or a
    // move-only `R`) use `take()`. For a value task, fatal if it was cancelled (no result) -
    // check `is_cancelled()` first; a cancelled `void` sync() simply returns.
    // LIFETIME: the returned reference names storage the block owns, so binding it to the
    // result of a temporary handle dangles at the end of the full-expression:
    //
    //   const R& bad = obj.async(fn).sync();   // the last handle dies; `bad` dangles
    //   auto ok = obj.async(fn).sync();        // the copy happens inside the expression
    //   R moved = obj.async(fn).take();        // or move the result out
    //
    // `Access_op::sync() &&` does return by value for the same spelling, and the asymmetry
    // is deliberate: an `Access_op` is single-owner (non-copyable, caller-owned storage), so
    // an rvalue op is provably the last owner and consuming it is sound. A `Task` is a
    // refcounted handle onto a shared block - an rvalue `Task` may be one of several live
    // copies, and moving the result out from under the others would break the "any number of
    // readers" contract. There is therefore no rvalue overload here; name the handle, or copy.
    // NOTE: `sync()` waits for this task to settle, not for work attached downstream -
    // `settle()` fires internal continuations after waking waiters (`notify_all`), so an
    // attached callback may still be running (or not yet started) when `sync()` returns.
    decltype(auto) sync()
    {
        detail::Task_control_block::sync_wait(core_);
        if constexpr (std::is_void_v<R>)
        {
            return;
        }
        else
        {
            if (core_->cancelled)
                ts::fatal("Task::sync() on a cancelled task; check is_cancelled() first");
            return *static_cast<const R*>(core_->result_ptr);
        }
    }

    // Blocks, then **moves** the result out - the single destructive consume (for ownership
    // handoff or a move-only `R`). Leaves the stored result moved-from, so it must be the last
    // consume (see the block's result-consumption contract); a second consume is fatal under
    // `TS_SAFETY_CHECKS` rather than silently handing back the hollow object. Fatal if the
    // task was cancelled.
    R take() requires (!std::is_void_v<R>)
    {
        detail::Task_control_block::sync_wait(core_);
        if (core_->cancelled)
            ts::fatal("Task::take() on a cancelled task; check is_cancelled() first");
#if TS_SAFETY_CHECKS
        if (core_->result_consumed.exchange(true, std::memory_order_acq_rel))
        {
            ts::fatal("Task::take() on a result already consumed - take()/try_take() moves the "
                      "result out, so it must be the last consume (use sync() for repeatable reads)");
        }
#endif
        return std::move(*static_cast<R*>(core_->result_ptr));
    }

    // The two cancellation-tolerant consumes. `sync()`/`take()` assert "this cannot be
    // cancelled" and fatal when it was, which is right for the common case (no token in
    // play) but punishes a caller for a state the callee chose - and there is no
    // check-then-take that is not a race. These two branch instead:
    //
    //   try_take()   - never blocks. Empty when the task is unsettled or cancelled, so it
    //                   is also legal inside a task (the non-blocking spelling of
    //                   `if (t.is_done()) v = t.sync();`).
    //   as_optional()-- `co_await t.as_optional()` waits, then yields empty on cancellation
    //                   instead of the fatal that `co_await t` raises.
    //
    // Both move the result out, like `take()`: the stored result is left moved-from, so
    // either must be the last consume - a later `take()` is fatal under `TS_SAFETY_CHECKS`
    // and a later `try_take()` reads empty. Neither exists for `void` - a void task has no
    // result to be missing, `is_done()` answers the first and awaiting a cancelled void task
    // already resumes normally, so `is_cancelled()` answers the second.
    std::optional<R> try_take() requires (!std::is_void_v<R>)
    {
        if (!core_ || !core_->ready.load(std::memory_order_acquire) || core_->cancelled)
            return std::nullopt;
#if TS_SAFETY_CHECKS
        // Already consumed reads as empty rather than fatal, matching `Access_op::try_take`:
        // this verb's whole shape is "answer, do not assert".
        if (core_->result_consumed.exchange(true, std::memory_order_acq_rel))
            return std::nullopt;
#endif
        return std::move(*static_cast<R*>(core_->result_ptr));
    }

    // Awaitable-only (there is nothing to wait on outside a coroutine that `sync()` does not
    // already do). See `operator co_await` in coroutine_support.h.
    detail::Optional_awaitable<R> as_optional() const noexcept requires (!std::is_void_v<R>)
    {
        return detail::Optional_awaitable<R>{ core_ };
    }

protected:
    detail::Task_control_block* control() const noexcept { return core_.get(); }

private:
    template<typename R2>
    friend detail::Task_ptr detail::core_of(const Task<R2>&) noexcept;
    template<typename R2>
    friend Task<R2> detail::task_from_core(detail::Task_ptr) noexcept;

    detail::Task_ptr core_;
};

namespace detail
{

template<typename R>
Task_ptr core_of(const Task<R>& t) noexcept { return t.core_; }

template<typename R>
Task<R> task_from_core(Task_ptr core) noexcept { return Task<R>(std::move(core)); }

} // namespace detail

namespace detail
{

// Shared builder for the bare-task entry point (`ts::launch`). The body is passed raw and
// runs under an empty access context (the same no-current-task case a blue-boundary launch
// installs): a detached launch's handle may be dropped, so the launcher's grant cannot be
// guaranteed to outlive the child, and an undeclared touch of the launcher's guarded data
// faults deterministically on the first access rather than racing the launcher's closing
// grant window (docs/internals/coroutine-first.md §2). The raw body keeps its shape (a
// trailing-`Cancellation_token` overload is preserved), so `Executable::run` forwards the
// token either way. To fan work out over a node's owned data, use `ts::parallel_for` (its
// helpers inherit the caller's grant) or acquire fresh via `obj.async` / `co_await obj.access`.
template<typename Fn>
auto build_bare_task(Fn&& fn, Dispatch_options opts, std::source_location site)
{
    using R = detail::Task_result_t<Fn>;
    Task_ptr core = make_executable<R>(std::forward<Fn>(fn), std::move(opts.token));
    core->flags.priority = detail::resolved_priority(opts.priority);
    set_task_name(core, named_from(opts, site));
    submit_ready(core);
    return Task<R>(core);
}

} // namespace detail

// Launch a standalone task on the scheduler - a bare functor with no access target (the
// primitive `async` for work that touches no guarded object). Returns a `Task<R>`; a
// `Dispatch_options{token, priority}` makes it skippable before it runs and sets its queue
// position. Dispatches through the `submit_ready` bridge (so this stays scheduler-
// independent). The launched task inherits nothing from the launcher - its handle may be
// dropped (the detached case), so the launcher's grant cannot be guaranteed to outlive the
// child; a body that touches the launcher's guarded data faults as undeclared access. To
// touch the launcher's data, use `ts::parallel_for` (its helpers inherit the caller's grant)
// or acquire fresh via `obj.async(...)` / `co_await obj.access(...)`.
// (Deduced return - `Task<Task_result_t<Fn>>` - rather than a trailing return type:
// the trailing form substitutes during overload resolution, before the constraint is
// checked, so a wrong body shape would hard-error inside `Task_result` instead of
// failing the `Task_body` gate.)
// `site` is the naming boundary (ts/named.h): a defaulted `source_location` captures the
// caller, so it must sit on the outermost function the user calls - `launch` - and the
// resulting `Named` is passed down explicitly, never re-defaulted in a helper.
template<typename Fn>
    requires detail::Task_body<Fn>
auto launch(Fn&& fn, Dispatch_options opts = {},
            std::source_location site = std::source_location::current())
{
    return detail::build_bare_task(std::forward<Fn>(fn), std::move(opts), site);
}

// Declares that something the task system is waiting on will be completed by a thread the
// scheduler does not own - an OS I/O completion, a GPU fence, a `Signal` triggered from a
// dedicated engine thread, a `Frame_gate`'s next `open()`. Hold one for as long as that
// wakeup is outstanding.
//
// This is the deadlock net's escape (`Rule::deadlock_net`, docs/internals/waiting-rule-policy.md §7).
// The net fires when the scheduler is quiescent and nothing is registered here - so a
// forgotten registration produces a false deadlock report, which is why the report names
// this type. It is not suppressible by scope: the net observes the whole process, so there
// is no call site to attribute a relaxation to; a build drops it with `TS_ENABLED_RULES`.
class External_wait
{
public:
    External_wait() noexcept
    {
#if TS_RULE_ON(TS_RULE_DEADLOCK_NET)
        detail::outstanding_external_waits.fetch_add(1, std::memory_order_acq_rel);
#endif
    }

    ~External_wait()
    {
#if TS_RULE_ON(TS_RULE_DEADLOCK_NET)
        detail::outstanding_external_waits.fetch_sub(1, std::memory_order_acq_rel);
#endif
    }

    External_wait(const External_wait&) = delete;
    External_wait& operator=(const External_wait&) = delete;
};

// Tune the deadlock net: how long the scheduler must be continuously quiescent, with nothing
// externally outstanding, before a blocked boundary waiter declares deadlock. 0 disables the
// net for this process (the whole-build switch is `TS_ENABLED_RULES`). Raise it if the
// program has legitimate blue-thread handoffs slower than the default two seconds.
inline void set_deadlock_net_window(std::chrono::milliseconds window) noexcept
{
#if TS_RULE_ON(TS_RULE_DEADLOCK_NET)
    detail::deadlock_net_window_ms.store(window.count(), std::memory_order_relaxed);
#else
    (void)window;
#endif
}

// A manually-completed synchronization point: a bodyless `Task<void>` (no work is
// scheduled or executed) that you `trigger()` by hand. It is both producer and
// consumer in one handle - the consumer side is inherited from `Task<void>`
// (`co_await` / `sync`, `is_done`), the producer side is `trigger()`. Copyable;
// copies share one control block. Used as a done-signal, a barrier / pipeline-phase
// gate, or an inter-task signal (the integrated equivalent of a manual-reset event
// / a promise+future fused). `trigger()` is idempotent (first call wins), so it is
// safe to trigger from multiple threads or more than once.
class Signal : public Task<void>
{
public:
    // Identified like any other task: by an explicit literal, else by its construction site
    // (`site` is the naming boundary - a defaulted `source_location` captures the caller).
    explicit Signal(const char* name = nullptr,
                    std::source_location site = std::source_location::current())
        : Task<void>(detail::make_bare_block())
    {
        detail::set_task_name(detail::core_of(*this), name != nullptr ? Named(name, site) : Named(site));
    }

    // Pins the block for the duration of `complete()`. `settle()` wakes its waiters before
    // running its own tail, and the thread it wakes may be the one holding the last `Signal`:
    // it returns from `sync()`, drops the handle, and frees the block while this thread is
    // still inside `settle()` reading members and destroying the condition variable. Every
    // other completer reaches `complete()` through a `Task_ptr` it holds; this one went
    // through the raw pointer, which is the use-after-free ThreadSanitizer reported the first
    // time the suite ran under it.
    void trigger()
    {
        detail::Task_ptr keep = detail::core_of(*this);
        keep->complete();
    }

    // Re-arm so it can be triggered again - a reusable barrier / phase gate. Precondition:
    // previously triggered and every waiter already returned from `sync()`/`co_await`.
    // `reset()` locks the block, so clearing the completion flag cannot race a waiter's read
    // (that race is real - it reproduces under TSan). Locking does NOT fix the lost-wakeup
    // window: a waiter that has not yet observed the trigger when `reset()` runs misses this
    // cycle and blocks until the next `trigger()`. If the waiters and the controller are not
    // externally sequenced, prefer `ts::Frame_gate`, which hands each phase a fresh single-use
    // signal and so has neither the race nor the lost wakeup.
    void reset()
    {
        control()->reset();
    }
};

} // namespace ts
