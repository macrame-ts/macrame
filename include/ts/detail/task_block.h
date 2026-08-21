#pragma once

// The task block layer behind `Task<R>` (ts/task.h): the fully monomorphic
// `Task_control_block` and its intrusive handle `Task_ptr`, the wrappers that compose a
// body and a result onto the block (`Executable`, `Block_backed`, `make_executable`), the
// dispatch and destroy trampolines, the block-naming helpers, the `current_task` TLS, the
// body-shape traits, and the scheduler-free seams the block dispatches through
// (`submit_ready`, `pipe_enter_first`, the deadlock-net internals). The design detail
// lives on the entities themselves; internals: docs/task-internals.md.

#include "ts/access.h"   // grant inheritance for launched/parallel_for sub-work (snapshot_access)
#include "ts/cancellation.h"   // the block holds a `Cancellation_token`
#include "ts/fatal.h"
#include "ts/named.h"   // ts::Named - debug identity for tasks (and nodes and objects)
#include "ts/priority.h"
#include "ts/rules.h"   // rule policy: which waiting-rule checks run, and their scoped opt-out
#include "ts/detail/ref_count.h"   // intrusive Ref_ptr / Ref_counted (preferred over shared_ptr)
#include "ts/detail/trace_owner.h"   // scheduler-free trace seam: owner inheritance + busy attribution

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#include <exception>   // the body seam reports `what()` when there is one (`invoke_user_body`)
#endif
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <type_traits>
#include <utility>
#include <vector>

namespace ts
{

template<typename R> class Task;   // defined in ts/task.h; named here by the `Is_task` trait

namespace detail
{

// Defined in scheduler.cpp (a scheduler-free seam, like `submit_ready`): before parking, a
// blocking wait drains the calling thread's pending worker-less-mode tasks - the awaited
// work may sit in the serial trampoline behind the waiter's own frame (a body that admitted
// work and then waits on it). No-op when nothing is pending (any scheduler mode).
void drain_serial_pending() noexcept;

#if TS_RULE_ON(TS_RULE_DEADLOCK_NET)
struct Task_control_block;
// Defined in guarded.cpp (where the global scheduler holder lives) - the scheduler-side
// half of the deadlock net, kept behind a plain function seam so this header stays free of
// the scheduler, exactly like `drain_serial_pending`. True when every worker is idle and
// every queue is empty; false when no scheduler has been created yet (a blue wait before
// the pool exists must not conjure one).
bool scheduler_quiescent() noexcept;
// Report the net firing. Defined in guarded.cpp so the message can name the blocked task.
[[noreturn]] void report_deadlock(const Task_control_block* waited_on) noexcept;
#endif

#if TS_RULE_ON(TS_RULE_IN_TASK_SYNC)
struct Task_control_block;
// Defined in guarded.cpp (it needs the `Pipe` layout this header deliberately lacks):
// report a `sync()`/`take()` issued from inside a task - fatal, with the sharp
// same-object message when the target is an access on an object the current context holds
// (a certain deadlock), the general message otherwise. Called by `sync_wait` for every
// in-task call, settled target or not (TODO 6.10).
[[noreturn]] void blocking_sync_diagnose(const Task_control_block* blk) noexcept;
#endif

// Report a body that let an exception escape (see `invoke_user_body`) - fatal. A handler runs
// after unwinding, so the throwing frames are already gone: the stack trace starts at the seam,
// and what locates the fault is the running task's identity plus the exception's own text
// (`what`; null when it does not derive from `std::exception`).
//
// Out of line for cost, not for layering - unlike `blocking_sync_diagnose` it needs nothing
// this header lacks. It is a cold path whose message formatting would otherwise be emitted
// into every seam instantiation, and it would put <cstdio> in a header every translation unit
// includes. The block layer is header-only, so the definition lives in guarded.cpp with the
// other diagnostics.
[[noreturn]] void escaped_exception_diagnose(const char* what) noexcept;

#if TS_RULE_ON(TS_RULE_DEADLOCK_NET)
// Work that only a non-worker thread can complete, currently outstanding (see
// `ts::External_wait`). The deadlock net's second predicate: quiescence with a nonzero count
// is a legitimate wait, not a deadlock.
inline std::atomic<int> outstanding_external_waits{ 0 };

// How long the scheduler must stay continuously quiescent before a blocked boundary waiter
// declares deadlock, and how many samples that window is split into. Long by design: a real
// deadlock is permanent, so latency is free, while a short window would fire on a legitimate
// blue-to-blue handoff that happens to be slow.
inline std::atomic<long long> deadlock_net_window_ms{ 2000 };
inline constexpr int deadlock_net_polls = 8;

inline std::chrono::milliseconds deadlock_net_window() noexcept
{
    return std::chrono::milliseconds(deadlock_net_window_ms.load(std::memory_order_relaxed));
}
#endif

struct Task_control_block;

// Intrusive strong-refcount ownership of a `Task_control_block`. The count + a `destroy`
// thunk live in the block (no separate control block), so a handle is one pointer (half a
// `shared_ptr`) - lighter in the successor/inline vectors and on every copy.
// The block's refcount starts at 0; the first `Task_ptr(&block)` brings it to 1. `inc`/`dec`
// are defined below the block (they touch its members).
void intrusive_inc(Task_control_block* p) noexcept;
void intrusive_dec(Task_control_block* p) noexcept;

// Tag for adopting an existing (already-counted) reference into a `Task_ptr` without an
// extra `inc` - pairs with `release()` to hand a ref across a raw-pointer boundary (e.g. a
// queued dispatch that carries the block as `void*`).
struct Adopt_ref {};

class Task_ptr
{
public:
    Task_ptr() noexcept = default;
    Task_ptr(std::nullptr_t) noexcept {}
    explicit Task_ptr(Task_control_block* p) noexcept : p_(p) { if (p_) intrusive_inc(p_); }
    Task_ptr(Task_control_block* p, Adopt_ref) noexcept : p_(p) {}   // adopt an existing ref (no inc)
    Task_ptr(const Task_ptr& o) noexcept : p_(o.p_) { if (p_) intrusive_inc(p_); }
    Task_ptr(Task_ptr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Task_ptr& operator=(const Task_ptr& o) noexcept
    {
        if (o.p_) intrusive_inc(o.p_);
        if (p_) intrusive_dec(p_);
        p_ = o.p_;
        return *this;
    }
    Task_ptr& operator=(Task_ptr&& o) noexcept
    {
        if (this != &o)
        {
            if (p_) intrusive_dec(p_);
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
    ~Task_ptr() { if (p_) intrusive_dec(p_); }

    // Relinquish ownership: return the raw pointer without decrementing (the ref is now the
    // caller's to manage - adopt it back with `Task_ptr(p, Adopt_ref{})`).
    Task_control_block* release() noexcept { auto* p = p_; p_ = nullptr; return p; }

    Task_control_block* get() const noexcept { return p_; }
    Task_control_block* operator->() const noexcept { return p_; }
    Task_control_block& operator*() const noexcept { return *p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    void reset() noexcept { if (p_) intrusive_dec(p_); p_ = nullptr; }

    friend bool operator==(const Task_ptr&, const Task_ptr&) noexcept = default;

private:
    Task_control_block* p_ = nullptr;
};

// Submit a block whose prerequisites are all met to run (defined in the scheduler
// layer; task.h stays scheduler-independent). Runs `execute` if it has a body, else
// `complete`s it.
void submit_ready(Task_ptr block);

// Like `submit_ready`, but the block is externally owned for its whole dispatch (its
// `flags.borrowed` is set - a graph node block, held by `Run_state` for the run), so the
// queue carries a borrowed raw pointer with no per-dispatch refcount inc/dec. Only valid
// when the owner provably outlives the dispatch. Defined in the scheduler layer.
void submit_borrowed(Task_control_block* blk);

// A task's per-pipe queue entry (full definition in ts/detail/pipe_link.h).
struct Pipe_link;

// Enter the task's first pipe (defined in the pipe layer, like `submit_ready` - a
// scheduler-free seam). Called when the pipe turns are the only unmet locks: by
// `release()` when the count drops to `pipe_count` (a task with ordinary prerequisites),
// or directly by a creation site with none (`async`, a data-ready graph node). The
// remaining pipes are entered by the sequential canonical cascade as earlier turns
// arrive (docs/pipe-rebase.md §0.2). `record`, when non-null, receives the block under
// the first pipe's mutex (the enqueue-and-record seam; see guarded.h).
void pipe_enter_first(Task_control_block* blk, Task_ptr* record = nullptr);

// --- Task control block ----------------------------------------------------
//
// The refcounted completion/dependency core behind a `Task<R>` handle. fully
// monomorphic - parameterized on nothing. The result type is erased behind a
// `void* result_ptr` (nullptr => no result: `void`/bodyless); a body, when present,
// hangs off an `Executable<Body,R>` wrapper (or a coroutine promise frame) that derives
// from the block, so a `Task_control_block*` recovers it with a `static_cast` (see
// docs/task-internals.md §2).
// Continuations receive `(result_ptr-or-nullptr, cancelled)` so they propagate a
// cancellation to their own subsequent. `settle()` is idempotent - the first settle
// wins (so a bodyless block can be triggered; see `Signal`). Result-consumption contract:
// `sync()` returns `const R&` (non-consuming), so any number of readers (`sync()` twice,
// several awaiters, N waiters) share one immutable-after-settle result; `take()` is the
// single destructive move (ownership handoff / move-only R). At most one mover, and last.
struct Task_control_block
{
    // Members are ordered for size: sub-8-byte fields clustered below the pointers (shared
    // padding), the two 32-bit atomics packed into one 8-byte slot. Cache-wise that concentrates
    // the whole dispatch-read set (`execute`/`result_ptr`/`pipe_links`/`flags`/`token`/`num_locks`)
    // in line 0, so starting a task touches one line - good locality - but line 0 also holds the
    // write-hot atomics (`refcount`, `num_locks`), so a handle copy/drop or an indegree decrement
    // on another core can false-share it during a parallel graph run. That tradeoff is unmeasured;
    // see docs/TODO.md 4.8 (cache-line alignment audit). See docs/task-internals.md §2.

    // Intrusive strong refcount (see `Task_ptr`) + `num_locks`. Two 32-bit atomics packed
    // adjacent = one 8-byte slot, no padding. `refcount` starts at 0 (first `Task_ptr` -> 1).
    // `num_locks`: below `execution_flag` it counts unmet prerequisites (gate execution);
    // once the body starts the flag is set and it counts pending nested tasks (gate
    // completion). See docs/task-internals.md §4/§7.
    static constexpr std::uint32_t execution_flag = 0x8000'0000u;
    std::atomic<std::uint32_t> refcount{ 0 };
    std::atomic<std::uint32_t> num_locks{ 0 };

    // Type-erased destroy thunk that deletes the enclosing wrapper (`Executable<Body,R>` /
    // `Graph_node_block` / a coroutine promise frame / a bare block).
    void (*destroy)(Task_control_block*) = nullptr;
    void* result_ptr = nullptr;        // -> the wrapper's stored R (set before complete), or null
    void (*execute)(const Task_ptr&) = nullptr;   // run the body (null => bodyless)
    // Fired once at `settle` (completed or cancelled), after continuations/successors.
    // Unlike a continuation it is not consumed, so a reused block (e.g. a re-armed graph
    // node) keeps it across runs - an alloc-free completion hook. Null for most tasks.
    void (*on_complete)(Task_control_block*) = nullptr;
    // The task's pipe entries (docs/pipe-rebase.md §0.2): an array of `pipe_count` links
    // embedded in the owning allocation (`Piped_executable` or the graph's per-node slab),
    // in canonical (ascending pipe-address) order. Null / 0 for a non-pipe task.
    // `pipe_count` doubles as the `release()` trigger threshold (pipes are entered when it
    // is the only remaining lock count - pipes-entered-last); `pipes_entered` is the
    // cascade's progress, and settle advances exactly links `[0, pipes_entered)`. Both
    // byte fields are written single-threaded (creation / the sequential cascade) and
    // published by the atomics around them.
    Pipe_link* pipe_links = nullptr;
    Cancellation_token token;          // checked by `execute` before running the body

    // --- one-byte cluster --------------------------------------------------------------
    // Each field is its own byte (distinct objects), so the lock-free atomics (`ready`,
    // `prereq_cancelled`) and the mutex-only bools (`completed`, `cancelled`) never share a
    // word - no read-modify-write straddles the lock/lock-free boundary. Clustered here so
    // they share trailing padding rather than each punching a hole between 8-byte members.
    // (Fusing `completed`+`cancelled` into a bitfield was measured to save 0 bytes - the
    // cluster's padding absorbs it - so they stay plain bools; simpler, no under-lock RMW.)
    std::atomic<bool> ready{ false };
    // Set when a prerequisite settled cancelled (`release` propagates the settle's cancel
    // state): a dependent (a graph successor) inherits the cancellation, so
    // `Executable::run` cancels instead of running the body. Harmless if set on a task
    // already executing (a cancelled nested child): the flag is only read at execution
    // start. Reset by `reset()` / graph re-arm.
    std::atomic<bool> prereq_cancelled{ false };
    // One-runner claim: set by the sole dispatch of a run before the body runs (`claim()`).
    // Every run has exactly one dispatch (`fetch_sub` values are unique, so one releaser
    // crosses zero), consumed before the run settles, and `reset` requires a settled run -
    // so a claim can never legitimately fail; the CAS is a machinery-bug detector (fatal
    // under `TS_SAFETY_CHECKS`, skip in shipping) across the inline / queue / node dispatch
    // routes, not a dedup mechanism. Cleared by `reset()` / graph re-arm.
    std::atomic<bool> body_claimed{ false };
    bool completed = false;            // mutex-only
    bool cancelled = false;            // mutex-only
    // Pipe fields (see `pipe_links`): entry count / trigger threshold, and cascade progress.
    std::uint8_t pipe_count = 0;
    std::uint8_t pipes_entered = 0;
    // Static dispatch properties, packed into one byte (all set at creation, never
    // mutated once the block is shared, so non-atomic is race-free). Read together at
    // dispatch. `run_inline` is graph-internal (`Graph_node::set_inline`).
    struct Flags
    {
        Priority priority : 2 = Priority::normal;   // queue position when dispatched
        bool run_inline : 1 = false;                // dispatch on the settling thread, not the queue
        // The block is externally owned for its whole dispatch lifetime (a graph node block,
        // held by `Run_state` for the run), so the queued dispatch carries a borrowed raw
        // pointer - no per-dispatch refcount inc/dec (`submit_borrowed`). Set once at
        // compile(); never true for async/coroutine blocks, which the queue must own.
        bool borrowed : 1 = false;
        // The block lives in caller storage (`Access_op`): the machinery must hold NO
        // reference past the moment the settle fires its continuations - a fired
        // continuation can resume the awaiting coroutine, whose frame owns the block, so a
        // straggling machinery ref would dec freed memory. Dispatch routes through the
        // borrowed (raw-pointer) queue path, and the pipe carries the entry without a ref
        // (`pipe_enter_link` / `fire_task_turn`); the block settles through the op's own
        // notify-under-lock settle, never the generic one. Unlike `borrowed` the owner is
        // not a longer-lived registry but the op itself, whose destructor blocks until the
        // access settles - that wait is the lifetime guarantee the refs used to be.
        bool caller_owned : 1 = false;
    };
    Flags flags;
    // -----------------------------------------------------------------------------------

    std::mutex mutex;
    std::condition_variable done_cv;   // wakes N waiters (a `Signal` is a barrier)

    // The one block whose completion lock this block holds - i.e. the parent a nested child
    // releases when it settles. A single slot, not a vector: `add_nested` is the sole producer
    // and it runs once per child (a coroutine node's frame and a nested graph run are each
    // attached once), so the fan-out here is structurally 0 or 1. A vector cost 24 bytes plus
    // a heap allocation on the first push for a link that is
    // always a single pointer. Double-nesting is rejected under safety checks rather than
    // silently dropped - see `add_nested`.
    Task_ptr nested_parent;
    std::vector<std::move_only_function<void(void*, bool)>> continuations;

#if TS_DEBUG_NAMES
    // Debug identity (`ts::Named`): the literal from the launching verb's options, else the
    // creation call site, else empty (a coroutine frame, whose promise sees no call site).
    // Diagnostics only - the circular-wait fatal, the quiescence dump - so it is fully gated
    // and shipping carries no bytes for it (the block is deliberately small; see TODO 4.7).
    // Cold tail placement: the hot clusters above keep their layout.
    Named name{ nullptr };
#endif

    // The block's identity, or the empty `Named` when names are compiled out - so callers
    // need no `#if` of their own.
    Named name_or_empty() const noexcept
    {
#if TS_DEBUG_NAMES
        return name;
#else
        return Named{ nullptr };
#endif
    }

    // Claim the body to run; true if this caller should run it. One dispatch per run and
    // re-arm only after settle mean failure is impossible in a correct program (see
    // `body_claimed`) - a failed CAS here is a duplicate or stale dispatch, i.e. a
    // machinery bug: fatal under `TS_SAFETY_CHECKS`, degrade to a skip in shipping.
    bool claim() noexcept
    {
        bool expected = false;
        const bool ok = body_claimed.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
#if TS_SAFETY_CHECKS
        if (!ok)
            ts::fatal("Task_control_block::claim failed - duplicate or stale dispatch (machinery bug)");
#endif
        return ok;
    }

    void complete() { settle(false); }
    void cancel()   { settle(true); }

    // A prerequisite or nested task settled. Decrement `blk`'s lock count and, when the
    // last lock drops, dispatch it (prerequisites met) or complete it (nested done).
    // `prereq_cancelled` carries whether the settling prerequisite was *cancelled*: the
    // dependent (a graph successor) then cancels instead of running (checked at execution
    // start). Non-prerequisite releases pass false. Nesting is ordering-only, so a
    // cancelled nested child sets the flag harmlessly (the parent is already executing;
    // the flag is only read before the body).
    static void release(const Task_ptr& blk, bool prereq_cancelled = false)
    {
        if (prereq_cancelled)
            blk->prereq_cancelled.store(true, std::memory_order_relaxed);
        std::uint32_t now = blk->num_locks.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (now == 0)
            dispatch_ready(blk);          // pre-execution: all locks (incl. pipe turns) met
        else if (now == execution_flag)
        {
            // Post-execution: the last nested child settled -> complete. A caller-owned
            // block (`Access_op`) must not complete through the generic settle (its settle
            // is the op's own notify-under-lock one, and its pipe advance is deferred to
            // completion - grants stay held across nested children); its settle thunk sits
            // on the otherwise-unused `on_complete` seam. Nothing here touches `blk` after
            // the call: the op may be destroyed inside it (a fired continuation resumes the
            // owner).
            if (blk->flags.caller_owned)
                blk->on_complete(blk.get());
            else
                blk->complete();
        }
        else if (now == blk->pipe_count)
            pipe_enter_first(blk.get());  // only pipe locks left -> enter line 0 (§5.5 pipes last)
        // The pipe branch is exact: pre-execution counts decrease monotonically (the
        // prerequisite set is frozen at launch), so `pipe_count` is crossed once, by exactly
        // one releaser (`fetch_sub` values are unique); a `pipe_count == 0` task takes the
        // `now == 0` dispatch instead, and post-execution counts sit above `execution_flag`,
        // far from any `pipe_count`.
    }

    // Per-thread FIFO trampoline for inline tasks: a ready inline task runs on this thread
    // (the one that settled its last prerequisite), driven iteratively so a chain of inline
    // tasks doesn't recurse (settle -> release -> execute -> settle -> ...) and blow the
    // stack. The first inline dispatch on a thread starts the drain; inline tasks made
    // ready during the drain just push and are picked up in order (head advances as the
    // vector grows). `clear()` at the end retains capacity -> no steady-state allocation.
    inline static thread_local std::vector<Task_ptr> inline_pending;
    inline static thread_local bool inline_draining = false;

    static void dispatch_ready(const Task_ptr& blk)
    {
        if (!blk->flags.run_inline)
        {
            // queued: the scheduler runs it (at blk->flags.priority). A borrowed block (a
            // graph node, owned by its `Run_state`) and a caller-owned one (an `Access_op`,
            // owned by its op storage) skip the dispatch-hop refcount.
            if (blk->flags.borrowed || blk->flags.caller_owned)
                submit_borrowed(blk.get());
            else
                submit_ready(blk);
            return;
        }
        inline_pending.push_back(blk);
        if (inline_draining)
            return;              // an active drain on this thread will run it
        inline_draining = true;
        for (std::size_t head = 0; head < inline_pending.size(); ++head)
        {
            Task_ptr b = std::move(inline_pending[head]);
            if (b->execute)
                b->execute(b);   // claims + runs the body on this thread
            else
                b->complete();
        }
        inline_pending.clear();   // retains capacity
        inline_draining = false;
    }

    void settle(bool cancel_)
    {
        // Graph node fast path (Opt 5): a node block has no external `sync()`/`co_await`
        // waiter (a node exposes no `Task<>` handle - `execute()` returns the run's `done`
        // handle, never a node), no continuations (`attach` only ever targets awaited `Task`
        // cores), and is never a nested child (`add_nested` attaches the coroutine frame / a
        // nested run as the child, with the node as parent - so `nested_parent` here stays
        // empty). So its completion needs none of the generic primitive: skip the mutex, the
        // `done_cv.notify_all()` that wakes nobody, and the always-empty continuations drain,
        // and fire `on_complete` directly under the atomic flags. The `num_locks` gating that
        // decides when a node settles is unchanged (in `run_graph_node` / `release`, before
        // this call), and exactly one settle occurs per run (`claim()` + a single threshold
        // crossing), so no idempotency lock is needed. The real cross-thread happens-before
        // for a dispatched successor is the scheduler queue / `remaining_deps` / the pipe -
        // never this block's mutex, which synchronized no one.
        if (flags.borrowed)
        {
#if TS_SAFETY_CHECKS
            if (nested_parent || !continuations.empty())
                ts::fatal("graph node block unexpectedly carries a nested parent or continuations "
                          "(the slim completion path assumes neither)");
#endif
            completed = true;
            cancelled = cancel_;
            ready.store(true, std::memory_order_release);
            if (on_complete)
                on_complete(this);
            return;
        }

        std::vector<std::move_only_function<void(void*, bool)>> conts;
        Task_ptr parent;
        void* r = nullptr;               // the result for continuations, captured under the lock
        {
            std::scoped_lock lock(mutex);
            if (completed)
                return;
            completed = true;
            cancelled = cancel_;
            // `ready` must be set under the same lock as `completed`: a `sync()` waits on
            // `completed` (acquiring this lock), then `reset()` checks `ready` lock-free. If
            // `ready` were stored after the lock, a sync() could observe `completed` and return
            // in the gap before `ready` was set, and the following `reset()` would wrongly
            // fatal ("reset() on a task that has not settled"). Under the lock, any observer of
            // `completed` also observes `ready`.
            ready.store(true, std::memory_order_release);
            conts = std::move(continuations);
            parent = std::move(nested_parent);
            // Read `result_ptr` for the continuations here, under the lock - not after the
            // notify below. Otherwise a re-armable block's waiter (woken by the notify) can
            // `reset()` + re-run and the new run overwrites `result_ptr` while this settle
            // tail still reads it (a data race under TSan). The moved-out `conts`/`parent`
            // are local, so firing them after the notify is fine.
            r = cancel_ ? nullptr : result_ptr;
        }
        done_cv.notify_all();   // `completed` set under the lock above: no lost wakeup
        // Waiters are woken before the settle tail runs, deliberately: the tail can be
        // unbounded (a continuation resumes a frame, `on_complete` releases the task's pipes
        // and thereby dispatches successors), and none of it is work the waiter is waiting
        // for. The consequence is a contract, not a bug: a returned `sync()` says this task
        // settled, never that its downstream work ran or that its pipe grants are released.
        // Teardown must therefore not infer pipe state from completion - `~Guarded` drains
        // the pipe itself (`Pipe::wait_until_idle`), which is what makes destroying an object
        // immediately after `sync()`ing its last access defined.
        for (auto& c : conts)
            c(r, cancel_);
        if (parent)
        {
            // Propagate cancellation to the gating parent. A caller-owned parent's link is
            // borrowed (see `add_nested`): read the flag into a local FIRST - the release can
            // complete the parent, whose owner may destroy it before `release` returns - then
            // defuse without a dec (`fire_task_turn`'s discipline).
            const bool parent_caller_owned = parent->flags.caller_owned;
            release(parent, cancel_);
            if (parent_caller_owned)
                parent.release();
        }
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
#if TS_RULE_ON(TS_RULE_DEADLOCK_NET)
        // `Rule::deadlock_net` (TODO 6.13) - Go's "all goroutines are asleep" check, with
        // the predicate Go lacks. A boundary waiter is the natural observer: it is already
        // blocked, so it costs nothing to have it look around. If, while it waits, the
        // scheduler is quiescent (every worker idle, every queue empty) and nothing is
        // registered as completable from outside the pool, then no thread and no queue can
        // ever settle what it is waiting for - progress is impossible.
        //
        // Sampling, not bookkeeping: no per-task counter, nothing on the hot path. One
        // sample would be worthless (a worker can sit between finding work and marking
        // itself busy), so the condition must hold continuously for the whole window.
        std::chrono::milliseconds window = deadlock_net_window();
        if (window.count() > 0)
        {
            const auto poll = window / deadlock_net_polls;
            int quiet = 0;
            while (!completed)
            {
                if (done_cv.wait_for(lock, poll, [this] { return completed; }))
                    return;
                bool quiescent = outstanding_external_waits.load(std::memory_order_acquire) == 0
                    && scheduler_quiescent();
                quiet = quiescent ? quiet + 1 : 0;
                if (quiet >= deadlock_net_polls)
                    report_deadlock(this);
            }
            return;
        }
#endif
        done_cv.wait(lock, [this] { return completed; });
    }

    // Re-arm this settled `Signal` block so it can be triggered again. The sole caller is
    // `Signal::reset` - graph nodes re-arm manually in `execute()`, ordered by the run's `done`
    // handle, never through here (static_task_graph.cpp). Takes the block mutex: `completed`/
    // `cancelled` are mutex-only, and a `Signal` waiter still returning from `wait()` reads
    // `completed` under that mutex, so clearing it lock-free is a data race (confirmed under
    // TSan). `nested_parent`/`continuations` were drained by `settle` and the result storage is
    // overwritten by the next run, so only the completion scalars reset here; `num_locks` is
    // left at 0. Precondition: settled, no in-flight run. Locking removes the race but not the
    // semantic lost-wakeup (a waiter that has not yet observed the trigger when `reset` clears
    // `completed` misses it) - `Signal::reset` documents that and points at `Frame_gate`.
    void reset()
    {
        std::scoped_lock lock(mutex);
        if (!completed)
            ts::fatal("Task_control_block::reset() on a task that has not settled");
        body_claimed.store(false, std::memory_order_relaxed);   // unclaimed for the next run
        completed = false;
        cancelled = false;
        prereq_cancelled.store(false, std::memory_order_relaxed);
        num_locks.store(0, std::memory_order_relaxed);
        ready.store(false, std::memory_order_release);
    }

    // Blocking wait for `blk` to settle (the blue-thread `sync()` path); defined after
    // `current_task` below (the in-task blocking fatal reads it).
    static void sync_wait(const Task_ptr& blk);
};

// `Task_ptr` refcount ops (block is complete here). `dec` at 0 runs the wrapper's `destroy`.
inline void intrusive_inc(Task_control_block* p) noexcept
{
    p->refcount.fetch_add(1, std::memory_order_relaxed);
}
// Bounded destruction trampoline. A block's destruction can release references to other
// blocks (a fused coroutine frame owns its `Task` parameters and promise state), so a
// deep chain destroyed recursively -
// dec -> destroy -> member dec -> destroy -> ... - overflows the stack (a 50k-deep await
// cascade did, under TSan). The last release pushes instead, and the outermost drain
// destroys iteratively (O(1) stack); the vector retains capacity, so the steady state
// allocates nothing.
// Trivially-destructible on purpose: releases can run during thread/process teardown
// (scheduler drain, TLS destructors), after a non-trivial thread-local's destructor would
// already have run - a `std::vector` here crashed at exit under TSan. The small buffer is
// deliberately leaked at thread exit.
struct Destroy_queue
{
    Task_control_block** items = nullptr;
    std::size_t size = 0;
    std::size_t cap = 0;
    bool draining = false;
};
inline thread_local Destroy_queue destroy_queue;

inline void intrusive_dec(Task_control_block* p) noexcept
{
    if (p->refcount.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;
    Destroy_queue& q = destroy_queue;
    if (q.size == q.cap)
    {
        std::size_t new_cap = q.cap == 0 ? 16 : q.cap * 2;
        auto** grown = new Task_control_block*[new_cap];
        for (std::size_t i = 0; i < q.size; ++i)
            grown[i] = q.items[i];
        delete[] q.items;
        q.items = grown;
        q.cap = new_cap;
    }
    q.items[q.size++] = p;
    if (q.draining)
        return;   // the active drain on this thread destroys it - don't recurse
    q.draining = true;
    for (std::size_t head = 0; head < q.size; ++head)
        q.items[head]->destroy(q.items[head]);   // may push more
    q.size = 0;   // buffer retained
    q.draining = false;
}

// CRTP base for a wrapper that embeds the block as a base subobject (`Executable`, the
// graph's node block): recovery from the type-erased `Task_control_block*` is a
// `static_cast` down to the derived wrapper - standard-defined for single non-virtual
// inheritance, unlike the first-member `reinterpret_cast` idiom this replaces (only
// conditionally supported for these non-standard-layout types). `from` is the recovery
// spelling; `install_destroy` wires the matching plain-`delete` thunk. A wrapper with its
// own destroy policy (a coroutine frame, a most-derived state deleted through a shared
// base) derives from `Task_control_block` directly and spells its own `static_cast`.
template<typename Derived>
struct Block_backed : Task_control_block
{
    static Derived* from(Task_control_block* c) noexcept { return static_cast<Derived*>(c); }
    void install_destroy() noexcept { destroy = [](Task_control_block* c) { delete static_cast<Derived*>(c); }; }
};

// A bare block (no result, no body - `Signal`): one allocation, destroyed as a plain
// `Task_control_block` when its refcount hits 0.
inline Task_ptr make_bare_block()
{
    auto* b = new Task_control_block();
    b->destroy = [](Task_control_block* c) { delete c; };
    return Task_ptr(b);   // refcount 0 -> 1
}

// Shared pre-settled void blocks for verbs whose fast path finishes the work in-call
// (`Deferred::commit` applying inline under a held grant): one static block per outcome,
// allocated once and never freed, so returning a `Task<void>` costs no allocation.
// Ordering contract: a pre-settled task provides no happens-before edge to its observer
// (it settled before the work it reports on) - callers that need ordering must go through
// the object's pipe, which orders; the settled handle only answers `is_done`/`sync` truthfully.
inline const Task_ptr& settled_void_core()
{
    static Task_ptr core = [] { Task_ptr b = make_bare_block(); b->complete(); return b; }();
    return core;
}
inline const Task_ptr& cancelled_void_core()
{
    static Task_ptr core = [] { Task_ptr b = make_bare_block(); b->cancel(); return b; }();
    return core;
}

// Record `name` on a freshly created block (no-op in shipping, where the block has no
// name field). Call at the public verb, with the `Named` that verb captured.
inline void set_task_name(const Task_ptr& core, const Named& name) noexcept
{
#if TS_DEBUG_NAMES
    core->name = name;
#else
    (void)core;
    (void)name;
#endif
}

// The `Named` a public verb records: its options' literal when given, at the site the verb
// captured. Both option aggregates carry a `const char* name`, so one helper serves all.
template<typename Options>
inline Named named_from(const Options& opts, const std::source_location& site) noexcept
{
    return opts.name != nullptr ? Named(opts.name, site) : Named(site);
}

// Display identity of a block for a diagnostic; never null. `buf` must outlive the use.
inline const char* task_name(const Task_control_block* blk, char* buf, std::size_t size) noexcept
{
#if TS_DEBUG_NAMES
    if (blk != nullptr)
        return named_display(blk->name, buf, size, "<unnamed task>");
#else
    (void)blk;
    (void)buf;
    (void)size;
#endif
    return "<task>";
}

// The task currently executing on this thread (for nested-task attachment). A `Task_ptr`
// (intrusive strong ref) so a nested child can register the parent via `nested_parent` and
// keep it alive until the child completes.
inline thread_local Task_ptr current_task;

// The running segment's implicit-scope child list: a coroutine frame installs its own
// per-frame list around each segment (see `coroutine_support.h`), so `detail::add_nested`
// records a nested graph run there for the graph's non-quiet-scope lending check. Null for
// functor bodies (counter-gated only) and outside tasks.
inline thread_local std::vector<Task_ptr>* current_scope_children = nullptr;

// A coroutine frame has no call site to capture (a promise sees the coroutine's arguments,
// not where it was called), so it inherits the identity of the task it was created inside
// - typically the graph node whose body it is. That is the identity a diagnostic wants
// anyway: the participant the user declared. Frames created outside any task stay unnamed.
inline void inherit_task_name(Task_control_block& core) noexcept
{
#if TS_DEBUG_NAMES
    if (current_task)
        core.name = current_task->name;
#else
    (void)core;
#endif
}

inline void Task_control_block::sync_wait(const Task_ptr& blk)
{
    // Worker-less mode: the awaited work (an async access, a released successor) may be
    // queued on this thread's serial trampoline behind the current frame - run it
    // before parking, or nothing ever would. No-op otherwise.
    drain_serial_pending();
#if TS_RULE_ON(TS_RULE_IN_TASK_SYNC)
    // `Rule::in_task_sync` (docs/coroutine-first.md §4.1, TODO 6.10): `sync()`/`take()`
    // inside a task is illegal whether or not the target has already settled. The check
    // used to fire only when the wait would genuinely park, which inverted its coverage:
    // a call whose target is usually settled never tripped in development, then parked a
    // worker on the one frame a prerequisite ran long, and in shipping it was compiled out
    // entirely. A check whose trigger is the hazard's timing inherits the hazard's
    // nondeterminism - so it triggers on the rule instead, deterministically on the first
    // execution of the path. `parallel_for` joins are structurally exempt (they wait on
    // group state directly, on provably running helpers, and never route here).
    if (current_task && rule_enforced(Rule::in_task_sync))
        blocking_sync_diagnose(blk.get());
#endif
    blk->wait();
}

// Empty for `void`, so an executable `void` task pays nothing for a result.
template<typename R> struct Result_storage { std::optional<R> result; };
template<> struct Result_storage<void> {};

// Invokes a user body at a task boundary. macrame itself never throws, and it does not carry
// an exception out of a body in any direction: the frames a body returns into are the
// library's own - a worker's dispatch loop, a pipe release, a coroutine resume - holding
// grants, lock counts and refcounts that unwinding would leave half updated, and they may be
// compiled with no exception support at all. An escaping exception is therefore reported and
// fatal. A body is free to use exceptions internally; it must handle them before returning.
//
// The handlers exist only where the calling translation unit has exceptions enabled; with them
// off this compiles to the invocation alone. If a program mixes both, the linker may keep
// either copy of an inlined seam, and the whole of that risk is which diagnostic a throwing
// body produces - this one, or the runtime's own bare terminate.
template<typename Fn, typename... Args>
decltype(auto) invoke_user_body(Fn&& fn, Args&&... args) noexcept
{
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
        return std::forward<Fn>(fn)(std::forward<Args>(args)...);
    }
    catch (const std::exception& e)
    {
        escaped_exception_diagnose(e.what());
    }
    catch (...)
    {
        escaped_exception_diagnose(nullptr);
    }
#else
    return std::forward<Fn>(fn)(std::forward<Args>(args)...);
#endif
}

// An executable task: the monomorphic block as a base subobject (`Block_backed`, so a
// `Task_control_block*` recovers the wrapper with a `static_cast`) + result storage +
// the body + a token. `run` is wired into the block's `execute`; the scheduler/pipe
// invokes it via `block->execute(block)`. The body lives here, its type erased behind the
// `execute` function pointer, so the block and everything downstream stay monomorphic.
template<typename Body, typename R>
struct Executable : Block_backed<Executable<Body, R>>
{
    Result_storage<R> storage;   // empty for void
    // The body lives in a union so `~Executable` does not auto-destroy it (TODO 7.3). The block
    // outlives the task's settle - its last ref is dropped on a worker, so a plain member would
    // keep the closure (and everything it captured: a `Recorder` into a journal, an escaped
    // reference) alive until then, past a `sync()` that already returned. `run()` destroys the
    // body in this typed context right after its last use, before the task settles, so captured
    // resources die before any waiter is woken.
    union { Body body; };
#if TS_SAFETY_CHECKS
    bool body_destroyed_ = false;   // set by `run()`; asserted in `~Executable` (never leaked)
#endif

    explicit Executable(Body b)
        : body(std::move(b))
    {}

    ~Executable()
    {
#if TS_SAFETY_CHECKS
        // The union body is destroyed exactly once, in `run()`. Reaching the dtor without that
        // is a "settled without running" regression that would leak the body - catch it.
        if (!body_destroyed_)
            ts::fatal("Executable destroyed without its body being run (TODO 7.3): body leaked");
#endif
    }

    // Destroy the body (and its captures) exactly once. Called on every run path that took
    // ownership of the run (post-`claim`), after the body's last use, before the task settles.
    void destroy_body() noexcept
    {
        std::destroy_at(&body);
#if TS_SAFETY_CHECKS
        body_destroyed_ = true;
#endif
    }

    static void run(const Task_ptr& c)
    {
        if (!c->claim())
            return;   // machinery bug (fatal under TS_SAFETY_CHECKS); skip in shipping
                      // - the losing dispatch never ran the body, so it must not destroy it.

        auto* self = Executable::from(c.get());
        if (c->token.is_cancel_requested() || c->prereq_cancelled.load(std::memory_order_acquire))
        {
            self->destroy_body();   // never invoked -> destroy before settle, no result to consume
            c->cancel();   // own token, or a prerequisite cancelled
            return;
        }

        // Switch the counter to completion-lock mode: the flag + a self-lock held for
        // the body. Nested tasks launched during the body add more locks.
        c->num_locks.store(Task_control_block::execution_flag + 1, std::memory_order_relaxed);
        auto prev = std::move(current_task);
        current_task = c;

        // The body may take the task's token (opt-in cooperative cancellation via a trailing
        // `Cancellation_token` parameter); pass it if so, otherwise invoke nullary.
        if constexpr (std::is_void_v<R>)
        {
            if constexpr (std::is_invocable_v<Body&, const Cancellation_token&>)
                invoke_user_body(self->body, c->token);
            else
                invoke_user_body(self->body);
        }
        else
        {
            if constexpr (std::is_invocable_v<Body&, const Cancellation_token&>)
                self->storage.result.emplace(invoke_user_body(self->body, c->token));
            else
                self->storage.result.emplace(invoke_user_body(self->body));
            c->result_ptr = &*self->storage.result;
        }

        current_task = std::move(prev);

        // Destroy the body (and its captures) now that it has run and any result is emplaced -
        // before the task settles, so a captured `Recorder`/reference cannot outlive a `sync()`
        // (TODO 7.3). Nested tasks launched during the body do not reference the body member.
        self->destroy_body();

        // Drop the self-lock. If it was the only remaining lock, no nested tasks are
        // pending -> complete now; otherwise the last nested task will complete us.
        if (c->num_locks.fetch_sub(1, std::memory_order_acq_rel) == Task_control_block::execution_flag + 1)
            c->complete();
    }
};

// Build an executable task; returns the handle core with `execute` wired up. Submit
// `[core]{ core->execute(core.get()); }` (to the scheduler or a pipe) to run it.
template<typename R, typename Body>
Task_ptr make_executable(Body&& body, Cancellation_token token)
{
    using Exec = Executable<std::decay_t<Body>, R>;
    auto* exec = new Exec(std::forward<Body>(body));
    exec->install_destroy();
    exec->execute = &Exec::run;
    exec->token = std::move(token);
    return Task_ptr(exec);   // refcount 0 -> 1, owns the wrapper
}

// A task body may opt into cooperative cancellation by declaring a trailing
// `Cancellation_token` parameter (`[](Cancellation_token t){...}` or, for `async`,
// `[](T& v, Cancellation_token t){...}`): `Executable::run` then passes the task's token
// so the body can poll `is_cancel_requested()` and early-out mid-execution. This is
// distinct from the pre-run skip (a token cancelled before the body starts skips it
// entirely); the parameter matters for cancellation that arrives while the body runs.
template<typename Fn>
inline constexpr bool takes_token_v = std::is_invocable_v<Fn&, const Cancellation_token&>;

// The body's result type, accounting for the optional trailing token parameter (a
// token-taking `fn` is not invocable with no args, so `invoke_result_t<Fn>` would be
// ill-formed). Specialized rather than `conditional_t` so only the valid branch is typed.
template<typename Fn, bool = takes_token_v<Fn>>
struct Task_result { using type = std::invoke_result_t<Fn&, const Cancellation_token&>; };
template<typename Fn>
struct Task_result<Fn, false> { using type = std::invoke_result_t<Fn&>; };
template<typename Fn> using Task_result_t = typename Task_result<std::decay_t<Fn>>::type;

// Dependent-false for a `static_assert` in a discarded `if constexpr` branch (the
// plain-`false` form is C++23 but not yet uniform across the toolchains we build on).
template<typename...> inline constexpr bool always_false = false;

// Whether `T` is a `ts::Task<R>` specialization (any `R`). Used where a body's result type
// selects behavior - e.g. a graph node body returning `Task<void>` is a coroutine node, and
// any other `Task<R>` result is rejected at compile time (static_task_graph.h).
template<typename T> struct Is_task : std::false_type {};
template<typename R> struct Is_task<Task<R>> : std::true_type {};
template<typename T> inline constexpr bool is_task_v = Is_task<T>::value;

// A bare task body (`ts::launch`): no parameters, or a single trailing
// `Cancellation_token` (cooperative cancellation, see `takes_token_v`). Gated at the
// entry points so a wrong shape rejects at the call site naming this concept instead
// of hard-erroring inside `Task_result`. (For a *generic* wrong body the token-arity
// probe still instantiates the body - same rendering as before the gate; only
// introspectable functors gain the clean rejection.)
template<typename Fn>
concept Task_body = std::invocable<std::decay_t<Fn>&>
    || std::invocable<std::decay_t<Fn>&, const Cancellation_token&>;

// Attach `child` as a nested task of the currently-executing task: that task will not
// complete until `child` settles (completed or cancelled). Fatal if there is no running
// task. Detail-level graph plumbing - the callers are the coroutine graph node's frame
// gating (static_task_graph.h) and a nested graph run (`add_nested(run.done)`,
// static_task_graph.cpp); nesting is a completion dependency, orthogonal to how the child runs.
inline void add_nested(Task_ptr child_core)
{
    if (!current_task)
        ts::fatal("detail::add_nested called outside a running task");
    // A caller-owned parent (`Access_op`) is linked BORROWED: the machinery must hold no ref
    // on it (Flags::caller_owned), and the op cannot settle - so cannot be legally destroyed
    // - before the child's release fires. The child's settle defuses symmetrically.
    Task_ptr parent = current_task->flags.caller_owned
        ? Task_ptr(current_task.get(), Adopt_ref{})
        : current_task;

    parent->num_locks.fetch_add(1, std::memory_order_relaxed);   // a completion lock on the parent

    // Record the child on the caller frame's implicit scope so the graph's non-quiet-scope
    // lending check (static_task_graph.cpp) can see a still-running nested graph run. A
    // coroutine frame installs the scope; functor bodies have none (nullptr), and a coroutine
    // node's frame is attached at the functor node's block, where the scope is likewise absent.
    if (current_scope_children != nullptr)
        current_scope_children->push_back(child_core);
    {
        std::scoped_lock lock(child_core->mutex);
#if TS_SAFETY_CHECKS
        // One gating parent per child, by construction at every call site (a coroutine node's
        // frame and a nested graph run are attached once each). The block carries a single slot
        // for that link, so a second attachment would silently drop the first parent's lock and
        // hang it forever.
        if (child_core->nested_parent)
            ts::fatal("detail::add_nested: this task already gates another parent");
#endif
        if (!child_core->completed)
        {
            child_core->nested_parent = std::move(parent);   // child releases parent when it settles
            return;
        }
    }
    // Child already settled -> release the lock now (same borrowed-link discipline as the
    // child-side settle: flag first, release, defuse).
    const bool parent_caller_owned = parent->flags.caller_owned;
    Task_control_block::release(parent);
    if (parent_caller_owned)
        parent.release();
}

} // namespace detail

} // namespace ts
