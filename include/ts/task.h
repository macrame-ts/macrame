#pragma once

#include "ts/access.h"   // grant inheritance for launched/nested sub-work (snapshot_access)
#include "ts/fatal.h"
#include "ts/priority.h"
#include "ts/detail/ref_count.h"   // intrusive Ref_ptr / Ref_counted (preferred over shared_ptr)
#include "ts/detail/trace_owner.h"   // scheduler-free trace seam: owner inheritance + busy attribution

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

// Defined in scheduler.cpp (a scheduler-free seam, like `submit_ready`): before parking, a
// blocking wait drains the calling thread's pending worker-less-mode tasks -- the awaited
// work may sit in the serial trampoline behind the waiter's own frame (a body that admitted
// work and then waits on it). No-op when nothing is pending (any scheduler mode).
void drain_serial_pending() noexcept;

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
    // this thread. Idempotent — the first call wins; later calls (and callbacks registered
    // after) are no-ops / fire immediately.
    void request_cancel();

    bool is_cancel_requested() const noexcept { return state_->requested.load(std::memory_order_acquire); }
    Cancellation_token token() const noexcept { return Cancellation_token(state_); }

private:
    detail::Ref_ptr<detail::Cancel_state> state_;
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

namespace detail
{

struct Task_control_block;

#if defined(TS_REUSE_FORENSICS)
// Diagnostic-only lock-free event ring for the stress_reuse flake hunt (see the
// `TS_REUSE_ONLY` driver in tsan/tsan_main.cpp). Compiled ONLY under TS_REUSE_FORENSICS;
// the default build sees no-op macros and is unaffected. Hooks filter on one `watched`
// block (the reused `dep`), record {event, a, b, tid} with relaxed atomics (minimal
// perturbation, TSan-clean by construction), and the driver dumps the tail on a capture.
namespace forensics
{
enum Event : std::uint32_t
{
    E_none = 0,
    E_round,          // a = round i, b = outer iter        (driver)
    E_reset,          // a = new gen                        (Task_control_block::reset)
    E_release,        // a = num_locks after, b = prereq_cancelled (release)
    E_submit,         // a = gen published to dispatch_arg  (submit_ready)
    E_pop,            // a = gen read from dispatch_arg     (run_block_dispatch)
    E_claim_ok,       // a = gen, b = path (1 queue, 2 retract, 3 other)
    E_claim_fail,     // a = gen, b = path
    E_cancel_branch,  // token/prereq-cancel skip taken     (Executable::run)
    E_body,           // a = log value read, b = run_state raw (driver body)
    E_prereq_body,    // a = i                              (driver prereq body)
    E_settle,         // a = cancel flag, b = already-completed (settle, under lock)
    E_retract_exec,   // a = gen about to execute inline    (retract)
    E_sync_ret,       // a = value returned, b = round i    (driver)
};

struct Entry
{
    std::atomic<std::uint64_t> a{ 0 };
    std::atomic<std::uint64_t> b{ 0 };
    std::atomic<std::uint32_t> id{ 0 };
    std::atomic<std::uint32_t> tid{ 0 };
};

inline constexpr std::uint32_t ring_size = 1u << 17;   // ~131k events, wraps
inline Entry ring[ring_size];
inline std::atomic<std::uint32_t> ring_idx{ 0 };
inline std::atomic<Task_control_block*> watched{ nullptr };
inline thread_local std::uint64_t claim_path = 0;   // 1 queue-dispatch, 2 retract, 3 other

inline std::uint32_t tid_hash() noexcept
{
    return static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffu);
}

inline void rec(Event id, std::uint64_t a, std::uint64_t b) noexcept
{
    std::uint32_t i = ring_idx.fetch_add(1, std::memory_order_relaxed) & (ring_size - 1);
    ring[i].a.store(a, std::memory_order_relaxed);
    ring[i].b.store(b, std::memory_order_relaxed);
    ring[i].tid.store(tid_hash(), std::memory_order_relaxed);
    ring[i].id.store(id, std::memory_order_relaxed);
}

inline void rec_if(const Task_control_block* c, Event id, std::uint64_t a, std::uint64_t b) noexcept
{
    if (c && c == watched.load(std::memory_order_relaxed))
        rec(id, a, b);
}
} // namespace forensics
#define TS_FORENSIC(c, ev, a, b) ::ts::detail::forensics::rec_if((c), ::ts::detail::forensics::ev, (a), (b))
#define TS_FORENSIC_G(ev, a, b) ::ts::detail::forensics::rec(::ts::detail::forensics::ev, (a), (b))
#define TS_FORENSIC_PATH(p) (::ts::detail::forensics::claim_path = (p))
#else
#define TS_FORENSIC(c, ev, a, b) ((void)0)
#define TS_FORENSIC_G(ev, a, b) ((void)0)
#define TS_FORENSIC_PATH(p) ((void)0)
#endif

// Intrusive strong-refcount ownership of a `Task_control_block`. The count + a `destroy`
// thunk live IN the block (no separate control block), so a handle is ONE pointer (half a
// `shared_ptr`) -- lighter in the successor/prerequisite/inline vectors and on every copy.
// The block's refcount starts at 0; the first `Task_ptr(&block)` brings it to 1. `inc`/`dec`
// are defined below the block (they touch its members).
void intrusive_inc(Task_control_block* p) noexcept;
void intrusive_dec(Task_control_block* p) noexcept;

// Tag for adopting an existing (already-counted) reference into a `Task_ptr` without an
// extra `inc` -- pairs with `release()` to hand a ref across a raw-pointer boundary (e.g. a
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

    // Relinquish ownership: return the raw pointer WITHOUT decrementing (the ref is now the
    // caller's to manage -- adopt it back with `Task_ptr(p, Adopt_ref{})`).
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
// `complete`s it. `gen` is the generation the CALLER captured when the block became
// ready (at/before the `num_locks` decrement that hit zero) -- never re-read at submit
// time; see `release` and `dispatch_arg` for the TOCTOU this closes.
void submit_ready(Task_ptr block, std::uint64_t gen);

// --- Task control block ----------------------------------------------------
//
// The refcounted completion/dependency core behind a `Task<R>` handle. FULLY
// MONOMORPHIC — parameterized on nothing. The result type is erased behind a
// `void* result_ptr` (nullptr => no result: `void`/bodyless); a body, when present,
// hangs off a `Result_block<R>`/executable wrapper that has `core` as its first
// member so a `Task_control_block*` aliases it (see docs/task-internals.md §2).
// Continuations receive `(result_ptr-or-nullptr, cancelled)` so they propagate a
// cancellation to their own subsequent. `settle()` is idempotent — the first settle
// wins (so a bodyless block can be triggered; see `Signal`). Result-consumption contract:
// `sync()` returns `const R&` (non-consuming), so any number of readers (`sync()` twice,
// `sync()` + `then`, N waiters) share one immutable-after-settle result; `take()` is the
// single destructive move (ownership handoff / move-only R). At most one mover, and last.
struct Task_control_block
{
    // NOTE: members are ordered for size, not logic -- the sub-8-byte fields are clustered
    // (below the 8-byte block) so they share padding instead of each punching a hole between
    // pointers/atomics, and the two 32-bit atomics sit together to fill one 8-byte slot.
    // sizeof shrank 336 -> 320 by this reorder alone (clang-cl x64). See docs/task-internals.md §2.

    // Intrusive strong refcount (see `Task_ptr`) + `num_locks`. Two 32-bit atomics packed
    // adjacent = one 8-byte slot, no padding. `refcount` starts at 0 (first `Task_ptr` -> 1).
    // `num_locks`: below `execution_flag` it counts unmet PREREQUISITES (gate execution);
    // once the body starts the flag is set and it counts pending NESTED tasks (gate
    // completion). See docs/task-internals.md §4/§7.
    static constexpr std::uint32_t execution_flag = 0x8000'0000u;
    std::atomic<std::uint32_t> refcount{ 0 };
    std::atomic<std::uint32_t> num_locks{ 0 };

    // Type-erased destroy thunk that deletes the enclosing wrapper (`Result_block<R>` /
    // `Executable<Body,R>` / `Graph_node_block` / a bare block).
    void (*destroy)(Task_control_block*) = nullptr;
    void* result_ptr = nullptr;        // -> the wrapper's stored R (set before complete), or null
    void (*execute)(const Task_ptr&, std::uint64_t generation) = nullptr;   // run the body (null => bodyless)
    // Fired once at `settle` (completed OR cancelled), after continuations/successors.
    // Unlike a continuation it is NOT consumed, so a reused block (e.g. a re-armed graph
    // node) keeps it across runs -- an alloc-free completion hook. Null for most tasks.
    void (*on_complete)(Task_control_block*) = nullptr;
    // One-runner claim + reuse generation fused into ONE atomic, so a stale dispatch and
    // a concurrent `reset()` can't be observed out of order (two separate atomics could,
    // letting a stale dispatch see the old generation but the new unclaimed state and
    // wrongly run). Bits [63:1] = generation (bumped by `reset`), bit [0] = body claimed.
    // A dispatch captures the generation it was queued for and calls `claim(gen)`: exactly
    // one caller (worker or retractor) wins per generation; a dispatch left stale by a
    // `reset` (retraction queues a duplicate the reset would otherwise let re-run the
    // body) fails the CAS and skips.
    std::atomic<std::uint64_t> run_state{ 0 };
    // Per-dispatch argument, published by the dispatcher right before handing the block to the
    // scheduler queue (release; the trampoline's load is the acquire) so the payload need not
    // ride in the 16-byte `Task_entry` (the work-stealing deque stores those as
    // `std::atomic<Task_entry>`, lock-free only at two words). Interpreted by the MATCHING
    // trampoline, so the meanings can't mix:
    //   - `run_block_dispatch` (bare-scheduler dispatch, `submit_ready`): the reuse GENERATION,
    //     captured by the releaser BEFORE its `num_locks` decrement (see `release` -- reading it
    //     after the decrement was a TOCTOU: a releaser delayed past the retract+reset of the run
    //     it released would stamp the NEXT generation, whose claim then succeeded with
    //     prerequisites still unmet) and published with a MONOTONIC-MAX CAS (a delayed releaser
    //     must not regress the slot below a newer round's publish -- that would orphan the newer
    //     round's dispatch and hang a non-retracting waiter). Every value in the slot is
    //     therefore a generation whose run genuinely released to zero, so a stale entry reads
    //     either its own generation (claim fails after a reset) or a newer READY one (safe
    //     early run; `claim` de-dups) -- `claim` stays the correctness gate.
    //   - `run_pipe_job_read/write` (pipe-job dispatch, `Guarded::async`): the owning `Pipe*`
    //     (a pipe block is created, dispatched once, and never `reset`, so its generation is
    //     always 0 and the slot is free to carry the pipe instead; plain store under the pipe
    //     mutex -- the monotonic-gen scheme above never touches pipe blocks, disjoint path).
    std::atomic<std::uint64_t> dispatch_arg{ 0 };
    Cancellation_token token;          // checked by `execute` before running the body

    // --- one-byte cluster --------------------------------------------------------------
    // Each field is its own byte (distinct objects), so the lock-free atomics (`ready`,
    // `prereq_cancelled`) and the mutex-only bools (`completed`, `cancelled`) never share a
    // word -- no read-modify-write straddles the lock/lock-free boundary. Clustered here so
    // they share trailing padding rather than each punching a hole between 8-byte members.
    // (Fusing `completed`+`cancelled` into a bitfield was measured to save 0 bytes -- the
    // cluster's padding absorbs it -- so they stay plain bools; simpler, no under-lock RMW.)
    std::atomic<bool> ready{ false };
    // Set when a PREREQUISITE settled cancelled (`release` propagates the settle's cancel
    // state). A result-consuming dependent (`then`) has no result to read, and an
    // ordering dependent (`after`) inherits the cancellation for consistency, so
    // `Executable::run` cancels instead of running the body. Harmless if set on a task
    // already executing (a cancelled nested child): the flag is only read at execution
    // start. Reset by `reset()` / graph re-arm.
    std::atomic<bool> prereq_cancelled{ false };
    bool completed = false;            // mutex-only
    bool cancelled = false;            // mutex-only
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
    // -----------------------------------------------------------------------------------

    std::mutex mutex;
    std::condition_variable done_cv;   // wakes N waiters (a `Signal` is a barrier)

    std::vector<Task_ptr> successors;      // decremented on settle
    std::vector<Task_ptr> prerequisites;   // backward links, for deep retraction
    std::vector<std::move_only_function<void(void*, bool)>> continuations;

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
        const bool ok = run_state.compare_exchange_strong(
            expected, (gen << 1) | 1u, std::memory_order_acq_rel, std::memory_order_relaxed);
#if defined(TS_REUSE_FORENSICS)
        forensics::rec_if(this, ok ? forensics::E_claim_ok : forensics::E_claim_fail,
                          gen, forensics::claim_path);
#endif
        return ok;
    }

    void complete() { settle(false); }
    void cancel()   { settle(true); }

    // A prerequisite or nested task settled. Decrement `blk`'s lock count and, when the
    // last lock drops, dispatch it (prerequisites met) or complete it (nested done).
    // `prereq_cancelled` carries whether the settling prerequisite was *cancelled*: a
    // dependent (`then`/`after`) then cancels instead of running (checked at execution
    // start). Non-prerequisite releases (the "not launched"/"not attached" lock) pass
    // false. Nesting is ordering-only, so a cancelled nested child sets the flag harmlessly
    // (the parent is already executing; the flag is only read before the body).
    static void release(const Task_ptr& blk, bool prereq_cancelled = false)
    {
        if (prereq_cancelled)
            blk->prereq_cancelled.store(true, std::memory_order_relaxed);
        // Capture the generation BEFORE the decrement. Our not-yet-released lock pins the
        // current run (it cannot start, so it cannot complete, so `reset()` cannot re-arm
        // it), so this read names exactly the run this lock belongs to. Reading it AFTER
        // the decrement is a TOCTOU: once the count hits zero the run can complete via
        // retraction and be re-armed, and a releaser preempted in that window would stamp
        // its dispatch with the NEXT generation -- which then claims a run whose
        // prerequisites are not yet met (proven by the stress_reuse forensics: `got == i-1`
        // with the current generation claimed; see tsan/reuse_hunt.sh).
        std::uint64_t gen = blk->generation();
        std::uint32_t now = blk->num_locks.fetch_sub(1, std::memory_order_acq_rel) - 1;
        TS_FORENSIC(blk.get(), E_release, now, prereq_cancelled ? 1 : 0);
        if (now == 0)
            dispatch_ready(blk, gen);     // pre-execution: prerequisites met -> queue, or run inline
        else if (now == execution_flag)
            blk->complete();              // post-execution: nested done -> complete (`gen` unused)
    }

    // Per-thread FIFO trampoline for inline tasks: a ready inline task runs on THIS thread
    // (the one that settled its last prerequisite), driven iteratively so a chain of inline
    // tasks doesn't recurse (settle -> release -> execute -> settle -> ...) and blow the
    // stack. The first inline dispatch on a thread starts the drain; inline tasks made
    // ready during the drain just push and are picked up in order (head advances as the
    // vector grows). `clear()` at the end retains capacity -> no steady-state allocation.
    // Each entry carries the GENERATION captured at release time (same TOCTOU as the queued
    // path: re-reading `generation()` at drain time could see a newer gen if the block was
    // retracted + re-armed between the push and the drain step).
    inline static thread_local std::vector<std::pair<Task_ptr, std::uint64_t>> inline_pending;
    inline static thread_local bool inline_draining = false;

    // `gen` is the generation captured by the caller when the block became ready (see
    // `release`); threaded through both dispatch routes, never re-read here.
    static void dispatch_ready(const Task_ptr& blk, std::uint64_t gen)
    {
        if (!blk->flags.run_inline)
        {
            submit_ready(blk, gen);   // queued: the scheduler runs it (at blk->flags.priority)
            return;
        }
        inline_pending.push_back({ blk, gen });
        if (inline_draining)
            return;              // an active drain on this thread will run it
        inline_draining = true;
        for (std::size_t head = 0; head < inline_pending.size(); ++head)
        {
            auto [b, g] = std::move(inline_pending[head]);
            if (b->execute)
                b->execute(b, g);   // claims + runs the body on this thread
            else
                b->complete();
        }
        inline_pending.clear();   // retains capacity
        inline_draining = false;
    }

    void settle(bool cancel_)
    {
        std::vector<std::move_only_function<void(void*, bool)>> conts;
        std::vector<Task_ptr> succs;
        std::vector<Task_ptr> prereqs;   // drop (no longer needed)
        void* r = nullptr;               // the result for continuations, captured under the lock
        {
            std::scoped_lock lock(mutex);
            TS_FORENSIC(this, E_settle, cancel_ ? 1 : 0, completed ? 1 : 0);
            if (completed)
                return;
            completed = true;
            cancelled = cancel_;
            // `ready` MUST be set under the same lock as `completed`: a `sync()` waits on
            // `completed` (acquiring this lock), then `reset()` checks `ready` lock-free. If
            // `ready` were stored after the lock, a sync() could observe `completed` and return
            // in the gap before `ready` was set, and the following `reset()` would wrongly
            // fatal ("reset() on a task that has not settled"). Under the lock, any observer of
            // `completed` also observes `ready`.
            ready.store(true, std::memory_order_release);
            conts = std::move(continuations);
            succs = std::move(successors);
            prereqs = std::move(prerequisites);
            // Read `result_ptr` for the continuations HERE, under the lock -- not after the
            // notify below. Otherwise a reusable task's `sync()` (woken by the notify) can
            // `reset()` + relaunch and the new run overwrites `result_ptr` while this settle
            // tail still reads it (a data race the reuse stress hits under TSan). The moved-out
            // `conts`/`succs` are local, so firing them after the notify is fine.
            r = cancel_ ? nullptr : result_ptr;
        }
        done_cv.notify_all();   // `completed` set under the lock above: no lost wakeup
        for (auto& c : conts)
            c(r, cancel_);
        for (auto& s : succs)
            release(s, cancel_);   // propagate cancellation to dependents
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
        prereq_cancelled.store(false, std::memory_order_relaxed);
        num_locks.store(0, std::memory_order_relaxed);
        ready.store(false, std::memory_order_release);
        TS_FORENSIC(this, E_reset, generation(), 0);
    }

    // Deep retraction: run the un-started part of `blk`'s dependency subtree inline on
    // the *calling* thread instead of parking on it — so a waiter under worker
    // exhaustion (nested fork-join) makes progress rather than deadlocking. Retract
    // `blk`'s prerequisites first (recursively), then, once its prerequisites are met
    // and it hasn't started, run its body inline. `execute` claims via `run_state`, so a
    // worker and a retractor never both run a body; non-retractable prerequisites
    // (pipe tasks, externally-triggered `Signal`s) are left to complete on their own.
    static void retract(const Task_ptr& blk)
    {
        if (!blk->flags.retractable || blk->ready.load(std::memory_order_acquire))
            return;

        std::vector<Task_ptr> prereqs;
        {
            std::scoped_lock lock(blk->mutex);
            prereqs = blk->prerequisites;   // snapshot (they clear as they settle)
        }
        for (const auto& p : prereqs)
            retract(p);

        // Reading `generation()` after the locks check is safe HERE (unlike `release`): the
        // retractor is a `sync()` caller of the CURRENT run, and only the resetter advances
        // the generation -- the builder contract (one run in flight; `reset()` only after the
        // prior run settled and was consumed) means no thread can be re-arming the block
        // while a same-run retractor is inside this call.
        if (blk->execute && blk->num_locks.load(std::memory_order_acquire) == 0)
        {
            TS_FORENSIC(blk.get(), E_retract_exec, blk->generation(), 0);
            TS_FORENSIC_PATH(2);
            blk->execute(blk, blk->generation());   // ready & not started -> run inline (no-op if a worker beat us)
            TS_FORENSIC_PATH(0);
        }
    }

    static void retract_or_wait(const Task_ptr& blk)
    {
        retract(blk);
        // Worker-less mode: the awaited work (a pipe job, a released successor) may be
        // queued on THIS thread's serial trampoline behind the current frame -- run it
        // before parking, or nothing ever would. No-op otherwise.
        drain_serial_pending();
        blk->wait();
    }
};

// `Task_ptr` refcount ops (block is complete here). `dec` at 0 runs the wrapper's `destroy`.
inline void intrusive_inc(Task_control_block* p) noexcept
{
    p->refcount.fetch_add(1, std::memory_order_relaxed);
}
inline void intrusive_dec(Task_control_block* p) noexcept
{
    if (p->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
        p->destroy(p);
}

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

// A bare block (no result, no body -- `Signal`, a `when_all` void join): one allocation,
// destroyed as a plain `Task_control_block` when its refcount hits 0.
inline Task_ptr make_bare_block()
{
    auto* b = new Task_control_block();
    b->destroy = [](Task_control_block* c) { delete c; };
    return Task_ptr(b);   // refcount 0 -> 1
}

// Make a fresh block for a `Task<R>`; returns the handle core plus (for a non-void R) a RAW
// pointer to the typed wrapper the producer stores into. The wrapper is owned by the block's
// intrusive refcount (`core`), so the raw pointer stays valid as long as any `Task_ptr` to
// the block does -- capture `core` (owning) alongside it, never the raw pointer alone.
template<typename R>
auto make_block()
{
    if constexpr (std::is_void_v<R>)
    {
        return make_bare_block();
    }
    else
    {
        auto* wrapper = new Result_block<R>();
        wrapper->core.destroy = [](Task_control_block* c) { delete reinterpret_cast<Result_block<R>*>(c); };
        return std::pair{ Task_ptr(&wrapper->core), wrapper };
    }
}

// The task currently executing on this thread (for nested-task attachment). A
// shared_ptr so a nested child can register the parent as its successor and keep it
// alive until the child completes.
inline thread_local Task_ptr current_task;

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

    static void run(const Task_ptr& c, std::uint64_t gen)
    {
        if (!c->claim(gen))
            return;   // claimed by another runner, or stale after a reset

        auto* self = reinterpret_cast<Executable*>(c.get());
        if (c->token.is_cancel_requested() || c->prereq_cancelled.load(std::memory_order_acquire))
        {
            TS_FORENSIC(c.get(), E_cancel_branch, gen, 0);
            c->cancel();   // own token, or a prerequisite cancelled (no result to consume)
            return;
        }

        // Switch the counter to completion-lock mode: the flag + a self-lock held for
        // the body. Nested tasks launched during the body add more locks.
        c->num_locks.store(Task_control_block::execution_flag + 1, std::memory_order_relaxed);
        auto prev = std::move(current_task);
        current_task = c;

        // The body may take the task's token (opt-in cooperative cancellation, see
        // `with_inherited_access`); pass it if so, otherwise invoke nullary.
        if constexpr (std::is_void_v<R>)
        {
            if constexpr (std::is_invocable_v<Body&, const Cancellation_token&>)
                self->body(c->token);
            else
                self->body();
        }
        else
        {
            if constexpr (std::is_invocable_v<Body&, const Cancellation_token&>)
                self->storage.result.emplace(self->body(c->token));
            else
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
Task_ptr make_executable(Body&& body, Cancellation_token token)
{
    using Exec = Executable<std::decay_t<Body>, R>;
    auto* exec = new Exec(std::forward<Body>(body));
    exec->core.destroy = [](Task_control_block* c) { delete reinterpret_cast<Exec*>(c); };
    exec->core.execute = &Exec::run;
    exec->core.token = std::move(token);
    return Task_ptr(&exec->core);   // refcount 0 -> 1, owns the wrapper
}

// A task body may opt into COOPERATIVE cancellation by declaring a trailing
// `Cancellation_token` parameter (`[](Cancellation_token t){...}` or, for `async`,
// `[](T& v, Cancellation_token t){...}`): `Executable::run` then passes the task's token
// so the body can poll `is_cancel_requested()` and early-out mid-execution. This is
// distinct from the pre-run skip (a token cancelled BEFORE the body starts skips it
// entirely); the parameter matters for cancellation that arrives WHILE the body runs.
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

// Wrap `fn` so the task runs under the access grant active where it was BUILT (see
// access.h): sub-work launched from a task body inherits the launcher's permissions and
// may touch the launcher's guarded data. The snapshot is by value, so it is valid after
// the launcher unwinds and on whatever worker (or retractor) runs the body; a top-level
// build (no active grant) captures nothing, so the scope is a no-op. Shared by
// `ts::launch` and `ts::task` so both the eager and the builder path inherit alike. If
// `fn` takes a trailing `Cancellation_token`, the wrapper does too (forwarded by
// `Executable::run` from the block's token), so the body can poll for cancellation.
template<typename R, typename Fn>
auto with_inherited_access(Fn&& fn)
{
    // Also snapshot the trace owner (graph-node index) so this sub-work's busy is attributed
    // to its owning node; `Trace_busy_scope` measures the body while the owner is live. Both
    // no-op under TS_PROFILING=0 (owner is -1, the scopes empty).
    if constexpr (takes_token_v<std::decay_t<Fn>>)
        return [fn = std::forward<Fn>(fn), ctx = snapshot_access(), owner = trace_owner()](const Cancellation_token& tok) mutable -> R
        {
            Trace_owner_scope trace_owner_scope(owner);
            Trace_busy_scope trace_busy_scope;
            Inherited_access_scope scope(ctx);
            return fn(tok);
        };
    else
        return [fn = std::forward<Fn>(fn), ctx = snapshot_access(), owner = trace_owner()]() mutable -> R
        {
            Trace_owner_scope trace_owner_scope(owner);
            Trace_busy_scope trace_busy_scope;
            Inherited_access_scope scope(ctx);
            return fn();
        };
}

// --- apply-style continuation detection -----------------------------------
//
// A `then` off a tuple-valued task may take the tuple by reference (`then([](tuple&
// t){...})`) or, apply-style, its elements unpacked (`then([](A& a, B& b){...})`). Any
// continuation shape may also opt into a trailing `Cancellation_token` (body-level
// early-out, like every other task body), so each detection comes in a plain and a
// token-taking flavor.

template<typename> inline constexpr bool is_tuple_v = false;
template<typename... Ts> inline constexpr bool is_tuple_v<std::tuple<Ts...>> = true;

// Invocable with the tuple's elements unpacked (`fn(Ts&...)`), plain or + a trailing token.
template<typename Fn, typename Tuple> struct Apply_invocable : std::false_type {};
template<typename Fn, typename... Ts>
struct Apply_invocable<Fn, std::tuple<Ts...>> : std::bool_constant<std::is_invocable_v<Fn, Ts&...>> {};

template<typename Fn, typename Tuple> struct Apply_invocable_tok : std::false_type {};
template<typename Fn, typename... Ts>
struct Apply_invocable_tok<Fn, std::tuple<Ts...>>
    : std::bool_constant<std::is_invocable_v<Fn, Ts&..., const Cancellation_token&>> {};

// The producer's result taken as a whole (`fn(R&)`), plain or + a trailing token.
template<typename Fn, typename R>
inline constexpr bool takes_whole_v =
    std::is_invocable_v<Fn, R&> || std::is_invocable_v<Fn, R&, const Cancellation_token&>;

// `then(fn)` unpacks when the producer is a tuple, `fn` does NOT take the tuple by
// reference (either arity), but it IS invocable with the tuple's elements (either arity).
template<typename Fn, typename R>
inline constexpr bool use_apply_v =
    is_tuple_v<R> && !takes_whole_v<Fn, R>
    && (Apply_invocable<Fn, R>::value || Apply_invocable_tok<Fn, R>::value);

template<typename Fn, typename Tuple, bool WithToken> struct Apply_result;
template<typename Fn, typename... Ts>
struct Apply_result<Fn, std::tuple<Ts...>, false> { using type = std::invoke_result_t<Fn, Ts&...>; };
template<typename Fn, typename... Ts>
struct Apply_result<Fn, std::tuple<Ts...>, true>
{ using type = std::invoke_result_t<Fn, Ts&..., const Cancellation_token&>; };

// `invoke_result_t<Fn, Args...>`, optionally with a trailing token appended. A struct (not
// `conditional_t`) so only the selected arity is instantiated — the other would be
// ill-formed when `fn` accepts just one of them.
template<typename Fn, bool WithToken, typename... Args> struct Invoke_result_tok
{ using type = std::invoke_result_t<Fn, Args...>; };
template<typename Fn, typename... Args> struct Invoke_result_tok<Fn, true, Args...>
{ using type = std::invoke_result_t<Fn, Args..., const Cancellation_token&>; };

// The control block behind a `Task` handle (used to wire prerequisites).
template<typename R>
Task_ptr core_of(const Task<R>& t) noexcept;

// Link `prereq` into `dependent`'s prerequisites as a RETRACTION HINT only — no lock
// count, no successor, completion stays whatever drives `dependent` (a continuation
// callback for `then`/`when_all`). It lets a blocking `sync()` on `dependent` walk to
// `prereq` and run it inline (deep retraction) when `prereq` is retractable and
// un-started, instead of parking a worker. Skipped if `prereq` already settled (nothing
// to retract; its callback has fired or will fire). Pushed under `prereq`'s mutex like
// `add_prerequisite`, and only before `dependent` is exposed, so it doesn't race
// `dependent`'s later settle/retract.
inline void add_retraction_hint(const Task_ptr& prereq,
                                const Task_ptr& dependent)
{
    std::scoped_lock lock(prereq->mutex);
    if (!prereq->completed)
        dependent->prerequisites.push_back(prereq);
}

// Register `succ` as a successor of `prereq` (bumping its lock count), unless `prereq`
// has already settled. `after` orders `succ` after `prereq` and propagates cancellation:
// a cancelled `prereq` still releases `succ`, but marks `succ->prereq_cancelled` so it
// settles cancelled rather than running (see `release` / `Executable::run`).
inline void add_prerequisite(const Task_ptr& prereq,
                             const Task_ptr& succ)
{
    std::scoped_lock lock(prereq->mutex);
    if (!prereq->completed)
    {
        succ->num_locks.fetch_add(1, std::memory_order_relaxed);
        prereq->successors.push_back(succ);
        succ->prerequisites.push_back(prereq);   // backward link, for deep retraction
    }
    else if (prereq->cancelled)
    {
        // Already settled cancelled -> `release` won't run, so propagate here: `succ`
        // settles cancelled instead of running (a `then` would otherwise read a missing
        // result). Under `prereq`'s mutex, so `cancelled` is consistent.
        succ->prereq_cancelled.store(true, std::memory_order_relaxed);
    }
}

} // namespace detail

// Dispatch options for a queued task body — a `then` continuation or a `Guarded` access
// (`access`/`async`). An aggregate, so it takes designated initializers at the call site:
// `t.then(fn, {.priority = Priority::high})`, `obj.access(fn, {.token = tok})`. `token` makes
// the body skippable before it runs (and, if the body declares a trailing `Cancellation_token`,
// is forwarded to it for a mid-run early-out). `run_inline` (used by `then`) runs the
// continuation on the thread that settles the producer instead of queueing it — same trade-offs
// as `Task_builder::set_inline`. Used by `then` and the task-builder path.
struct Task_options
{
    Cancellation_token token = {};
    Priority priority = Priority::normal;
    bool run_inline = false;
};

// Options for a `Guarded` access (`access` / `async`, single- and multi-object). Deliberately
// WITHOUT `run_inline`: the verb chooses inline-vs-enqueued (`access` runs inline when the queue
// is free via `pipe_try_inline`; `async` always enqueues), so a `run_inline` field here would be
// silently ignored — splitting the type makes passing it a compile error instead. `token` makes
// the body skippable before it runs (and is forwarded to a trailing-`Cancellation_token` body for
// a mid-run early-out); `priority` sets the queue position when enqueued.
// TODO(code-review): revisit whether Access_options and Task_options should stay separate or share
// a base once the review reaches the Guarded surface — the split is the conservative choice for now.
struct Access_options
{
    Cancellation_token token = {};
    Priority priority = Priority::normal;
};

// Dispatch options for launching a standalone task (`ts::launch`) or a nested one
// (`ts::nested`). Deliberately WITHOUT `run_inline`: `launch`/`nested` have no prerequisites,
// so there is no settling thread to inline onto — inline dispatch is a builder capability
// (`ts::task(fn).set_inline()`). `token` makes it skippable before it runs; `priority` sets
// its queue position.
struct Launch_options
{
    Cancellation_token token = {};
    Priority priority = Priority::normal;
};

// Handle to an async result. `sync()` blocks for the result (call once; may retract + run
// the task inline); `then()` chains a continuation that runs when this task completes.
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
    // `void` for a void one). The reference is valid while a handle (this `Task`, or the
    // `Task_builder` / another copy) keeps the block alive; `T r = t.sync()` copies within the
    // full-expression and is always safe. To *move* the result out (ownership handoff, or a
    // move-only `R`) use `take()`. For a value task, fatal if it was cancelled (no result) —
    // check `is_cancelled()` first; a cancelled `void` sync() simply returns.
    // NOTE: `sync()` waits for THIS task to settle, not for its downstream `then` continuations
    // — `settle()` fires continuations AFTER waking waiters (`notify_all`), so a continuation may
    // still be running (or not yet started) when `sync()` returns. To wait for a continuation,
    // `sync()` the `Task` that `then()` returns.
    decltype(auto) sync()
    {
        detail::Task_control_block::retract_or_wait(core_);
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

    // Blocks, then **moves** the result out — the single destructive consume (for ownership
    // handoff or a move-only `R`). Leaves the stored result moved-from, so it must be the last
    // consume (see the block's result-consumption contract). Fatal if the task was cancelled.
    R take() requires (!std::is_void_v<R>)
    {
        detail::Task_control_block::retract_or_wait(core_);
        if (core_->cancelled)
            ts::fatal("Task::take() on a cancelled task; check is_cancelled() first");
        return std::move(*static_cast<R*>(core_->result_ptr));
    }

    // Chains a continuation. Runs `fn` when this task completes; if this task is
    // cancelled (or `token` is cancelled when the continuation fires), `fn` is skipped
    // and the cancellation propagates to the returned task. For a non-void producer
    // `fn` receives the result by reference (or, for a tuple, apply-style); for void
    // it takes no argument.
    // The continuation `fn` may declare a trailing `Cancellation_token` in any of its
    // shapes (void / whole-result / apply-style) to poll for cancellation mid-run; the
    // token forwarded is the continuation's own (`opts.token`, checked at dispatch too).
    template<typename Fn>
    auto then(Fn&& fn, Task_options opts = {})
    {
        if constexpr (std::is_void_v<R>)
        {
            constexpr bool tok = detail::takes_token_v<std::decay_t<Fn>>;
            using R2 = detail::Task_result_t<Fn>;
            return chain<R2>([fn = std::forward<Fn>(fn)](void*, const Cancellation_token& t) mutable -> R2
            {
                if constexpr (tok) return fn(t);
                else return fn();
            }, std::move(opts));
        }
        else if constexpr (detail::use_apply_v<Fn, R>)
        {
            constexpr bool tok = detail::Apply_invocable_tok<std::decay_t<Fn>, R>::value
                                 && !detail::Apply_invocable<std::decay_t<Fn>, R>::value;
            using R2 = typename detail::Apply_result<std::decay_t<Fn>, R, tok>::type;
            return chain<R2>([fn = std::forward<Fn>(fn)](void* r, const Cancellation_token& t) mutable -> R2
            {
                if constexpr (tok)
                    return std::apply([&fn, &t](auto&... xs) -> R2 { return fn(xs..., t); }, *static_cast<R*>(r));
                else
                    return std::apply(fn, *static_cast<R*>(r));
            }, std::move(opts));
        }
        else
        {
            constexpr bool tok = std::is_invocable_v<std::decay_t<Fn>&, R&, const Cancellation_token&>
                                 && !std::is_invocable_v<std::decay_t<Fn>&, R&>;
            using R2 = typename detail::Invoke_result_tok<std::decay_t<Fn>&, tok, R&>::type;
            return chain<R2>([fn = std::forward<Fn>(fn)](void* r, const Cancellation_token& t) mutable -> R2
            {
                if constexpr (tok) return fn(*static_cast<R*>(r), t);
                else return fn(*static_cast<R*>(r));
            }, std::move(opts));
        }
    }

protected:
    detail::Task_control_block* control() const noexcept { return core_.get(); }

private:
    template<typename R2>
    friend detail::Task_ptr detail::core_of(const Task<R2>&) noexcept;

    // A continuation is a PROPER scheduled task (an `Executable`), not an inline callback:
    // its body runs `produce(producer's result)`, dispatched through the normal prerequisite
    // path when the producer settles -- queued by default (so the scheduler can slot
    // higher-priority work between a task and its continuation), inline opt-in. The producer
    // is a real `num_locks` prerequisite (so a blocking wait / retraction doesn't run this
    // task before the result exists, and `prerequisites` makes it deep-retractable). A
    // *cancelled* producer still dispatches us (cancellation propagates via
    // `prereq_cancelled`), and since there is then no result to consume, `Executable::run`
    // cancels this task instead of running the body. `produce` takes the producer's
    // `result_ptr` (null for a void producer); the producer stays alive as our prerequisite.
    template<typename R2, typename Produce>
    Task<R2> chain(Produce produce, Task_options opts)
    {
        auto producer = core_;
        auto next = detail::make_executable<R2>(
            [producer, produce = std::move(produce)](const Cancellation_token& tok) mutable -> R2
            { return produce(producer->result_ptr, tok); },
            std::move(opts.token));
        next->flags.retractable = true;
        next->flags.priority = opts.priority;
        next->flags.run_inline = opts.run_inline;
        next->num_locks.store(1, std::memory_order_relaxed);   // "not attached" lock
        detail::add_prerequisite(producer, next);              // wait for the producer to settle
        detail::Task_control_block::release(next);             // drop the lock -> dispatch when ready
        return Task<R2>(std::move(next));
    }

    detail::Task_ptr core_;
};

namespace detail
{

template<typename R>
Task_ptr core_of(const Task<R>& t) noexcept { return t.core_; }

} // namespace detail

// A configured-but-not-launched task: attach prerequisites, then `launch()`. Built by
// `ts::task(fn)`; owns the executable block until launched. `launch()` removes the
// "not launched" lock, so the task runs once every prerequisite has settled.
//
// The builder is also the **reusable** handle: it retains the block after `launch()`
// (which hands out a `Task<R>` aliasing the same block, but the builder keeps its own
// reference), so a body + result storage allocated once can be re-run many times. The
// pattern is `t.reset().after(...).launch(); r = t.sync();` — `reset()` re-arms the block
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

    // Set the cancellation token for the task. Call BEFORE the first `launch()` — the token
    // is fixed dispatch config, not per-run: `launch()` must NOT rewrite it, because a
    // *reused* block can have a prior run's worker still reading `token` (in `Executable::run`)
    // when the next round would write it, and that read/write is unsynchronized across the
    // reuse×retraction path — a data race. Setting it once, before any dispatch, is
    // happens-before the read. To re-run under a different token, make a fresh `ts::task`.
    Task_builder& token(Cancellation_token t)
    {
        core_->token = std::move(t);
        return *this;
    }

    Task<R> launch()
    {
        detail::Task_control_block::release(core_);   // remove the launch lock
        return Task<R>(core_);
    }

    // Re-arm for another run: re-arm the block and restore the "not launched" lock, so
    // `after(...).launch()` runs it again. Precondition: the prior run settled and its
    // result was consumed (one run in flight). Chain: `t.reset().after(x).launch()`.
    // The cancellation `token` is NOT reset — it is fixed dispatch config (set once via
    // `.token()` before the first launch) and carries over every reuse round; `reset()`
    // cannot swap it. Since cancellation is one-way, a reused task whose token was cancelled
    // stays cancelled on every re-run — for a fresh cancellation scope, make a new `ts::task`.
    // (Rewriting `token` per run would race a prior run's worker still reading it — see the
    // `.token()` note.)
    Task_builder& reset()
    {
        core_->reset();
        core_->num_locks.store(1, std::memory_order_relaxed);   // the "not launched" lock
        return *this;
    }

    // Read this run's result by `const&` (see `Task<R>::sync`) / move it out (`take`) / query
    // completion. The builder is the handle; equivalently `launch()`'s returned `Task<R>` can
    // be used. The block outlives the temporary `Task` here (the builder's `core_` keeps it).
    decltype(auto) sync() { return Task<R>(core_).sync(); }
    R take() requires (!std::is_void_v<R>) { return Task<R>(core_).take(); }
    bool is_done() const noexcept { return Task<R>(core_).is_done(); }
    bool is_cancelled() const noexcept { return Task<R>(core_).is_cancelled(); }

private:
    template<typename Fn> friend auto task(Fn&& fn);

    explicit Task_builder(detail::Task_ptr core) noexcept
        : core_(std::move(core))
    {}

    detail::Task_ptr core_;
};

// Configure a standalone task (body + prerequisites) to launch later. `fn` takes no
// arguments (a bare scheduler task). Runs once all prerequisites (added via
// `.after(...)`) have settled. Inherits the launcher's access grant (like `ts::launch`),
// so a builder task used as nested sub-work may touch the parent's guarded data.
template<typename Fn>
auto task(Fn&& fn)
{
    using R = detail::Task_result_t<Fn>;
    auto core = detail::make_executable<R>(detail::with_inherited_access<R>(std::forward<Fn>(fn)), {});
    core->num_locks.store(1, std::memory_order_relaxed);   // the "not launched" lock
    core->flags.retractable = true;   // bare scheduler task: safe to run inline from a waiter
    return Task_builder<R>(std::move(core));
}

// Launch a standalone task on the scheduler — a bare functor with no access target (the
// primitive `async` for work that touches no guarded object). Returns a `Task<R>`; a
// `Launch_options{token, priority}` makes it skippable before it runs and sets its queue
// position. Dispatches through the `submit_ready` bridge (so this stays scheduler-
// independent) and inherits the launcher's access grant, so sub-work launched from a task
// body may touch the launcher's data.
template<typename Fn>
auto launch(Fn&& fn, Launch_options opts = {})
    -> Task<detail::Task_result_t<Fn>>
{
    using R = detail::Task_result_t<Fn>;
    auto core = detail::make_executable<R>(detail::with_inherited_access<R>(std::forward<Fn>(fn)), std::move(opts.token));
    core->flags.retractable = true;   // bare scheduler task (no pipe binding): safe to run inline from a waiter
    core->flags.priority = opts.priority;
    detail::submit_ready(core, core->generation());   // fresh block, pre-dispatch: gen 0, race-free read
    return Task<R>(core);
}

// Attach `child` as a NESTED task of the currently-executing task: that task will not
// complete until `child` settles (completed or cancelled). Call from within a task
// body (async / launch / task); fatal if there is no running task. `child` can be
// launched any way — nesting is a completion dependency, orthogonal to how it runs.
template<typename R>
void add_nested(const Task<R>& child)
{
    detail::Task_ptr parent = detail::current_task;
    if (!parent)
        ts::fatal("add_nested called outside a running task");

    parent->num_locks.fetch_add(1, std::memory_order_relaxed);   // a completion lock on the parent

    detail::Task_ptr child_core = detail::core_of(child);
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
auto nested(Fn&& fn, Launch_options opts = {})
    -> Task<detail::Task_result_t<Fn>>
{
    auto t = launch(std::forward<Fn>(fn), std::move(opts));
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

// A `when_all` join's shared state: one intrusive allocation shared by every prerequisite's
// continuation + the finish.
// `next_core` (owned) keeps the join block alive; for a value join the block is a
// `Result_block<Result>` that `next_core` aliases, recovered by `reinterpret_cast` in `finish`.
template<typename Slots, typename Result>
struct Join_state : Ref_counted<Join_state<Slots, Result>>
{
    Slots slots;
    std::atomic<int> remaining;
    std::atomic<bool> any_cancelled{ false };   // set if any prerequisite settled cancelled
    Task_ptr next_core;

    explicit Join_state(int n) : remaining(n) {}

    // A prerequisite settled: store its result (or flag cancel), decrement; the last to settle
    // runs `finish`.
    template<int Slot, typename R>
    void settle_one(void* r, bool cancelled)
    {
        if (cancelled)
            any_cancelled.store(true, std::memory_order_relaxed);
        else if constexpr (!std::is_void_v<R>)
            std::get<Slot>(slots).emplace(std::move(*static_cast<R*>(r)));   // move out of the prerequisite
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
            finish();
    }

    void finish()
    {
        if (any_cancelled.load(std::memory_order_relaxed))
        {
            next_core->cancel();   // some slots empty (cancelled prereqs) -> no complete tuple
            return;
        }
        if constexpr (!std::is_void_v<Result>)
        {
            auto* wrapper = reinterpret_cast<Result_block<Result>*>(next_core.get());
            [&]<std::size_t... J>(std::index_sequence<J...>)
            {
                wrapper->store(Result(std::move(*std::get<J>(slots))...));   // move (move-only ok)
            }(std::make_index_sequence<std::tuple_size_v<Slots>>{});
        }
        next_core->complete();
    }
};

template<int Slot, typename R, typename Join>
void when_all_attach_one(Task<R> prereq, const Task_ptr& next_core, Ref_ptr<Join> state)
{
    // Attach directly to the prerequisite (NOT via `.then`, which skips its continuation
    // on cancellation and would leave the join's `remaining` counter stuck above 0 -- the
    // join would never settle). Each continuation holds one `Ref_ptr<Join_state>`.
    Task_ptr prereq_core = core_of(prereq);
    prereq_core->attach(
        [state = std::move(state)](void* r, bool cancelled) mutable
        {
            state->template settle_one<Slot, R>(r, cancelled);
        });
    add_retraction_hint(prereq_core, next_core);   // deep-retractable: sync() can run each prereq inline
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
    using Join = detail::Join_state<Slots, Result>;

    // The join block: a bare block for a void result, else a `Result_block<Result>` whose
    // handle the `Join_state` recovers by aliasing (see `Join_state::finish`).
    detail::Task_ptr next_core;
    if constexpr (std::is_void_v<Result>)
        next_core = detail::make_bare_block();
    else
        next_core = std::get<0>(detail::make_block<Result>());
    next_core->flags.retractable = true;   // a blocking sync() can retract the (retractable) prerequisites

    // ONE allocation for all the join's shared state (was four `make_shared`).
    auto state = detail::make_ref<Join>(static_cast<int>(sizeof...(Rs)));
    state->next_core = next_core;

    constexpr auto slot = detail::when_all_slots<Rs...>();
    auto prereqs = std::make_tuple(std::move(prerequisites)...);

    [&]<std::size_t... I>(std::index_sequence<I...>)
    {
        (detail::when_all_attach_one<slot[I]>(std::get<I>(std::move(prereqs)), next_core, state), ...);
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
// safe to trigger from multiple threads or more than once. As with any `Task`, `sync()`
// returns when the signal is triggered — it does NOT wait for `then` continuations (they
// fire after the waiter is woken); `sync()` the `Task` a `then()` returns to await one.
class Signal : public Task<void>
{
public:
    Signal()
        : Task<void>(detail::make_bare_block())
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
