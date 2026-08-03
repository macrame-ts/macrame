#pragma once

#include "ts/access.h"   // grant inheritance for launched/nested sub-work (snapshot_access)
#include "ts/fatal.h"
#include "ts/priority.h"
#include "ts/detail/ref_count.h"   // intrusive Ref_ptr / Ref_counted (preferred over shared_ptr)
#include "ts/detail/trace_owner.h"   // scheduler-free trace seam: owner inheritance + busy attribution

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ts
{

template<typename R> class Task;

// Cooperative cancellation. A `Cancellation_source` owns the request flag; hand its
// `token()` to `async`/`launch`/`Static_task_graph::execute`. Cancellation is checked
// when a task/node is about to run (not-yet-started work is skipped) and propagates
// down awaiting coroutines and graph successors as a completion state (see `is_cancelled`).
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

#if TS_SAFETY_CHECKS
struct Task_control_block;
// Defined in guarded.cpp (it needs the `Pipe` layout this header deliberately lacks):
// diagnose a blocking `sync()` on non-retractable work issued under an access scope -- a
// `TS_ENSURE` failure with the sharp same-object message when the target is an async access
// on an object the current context holds (certain deadlock), the general never-block warning
// otherwise. Called by `retract_or_wait` only when the wait is genuinely about to park.
void blocking_sync_diagnose(const Task_control_block* blk) noexcept;
#endif

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

// A task's per-pipe queue entry (full definition in ts/detail/pipe_link.h).
struct Pipe_link;

// Enter the task's first pipe (defined in the pipe layer, like `submit_ready` -- a
// scheduler-free seam). Called when the pipe turns are the only unmet locks: by
// `release()` when the count drops to `pipe_count` (a task with ordinary prerequisites),
// or directly by a creation site with none (`async`, a data-ready graph node). The
// remaining pipes are entered by the sequential canonical cascade as earlier turns
// arrive (docs/pipe-rebase.md §0.2). `record`, when non-null, receives the block under
// the first pipe's mutex (the enqueue-and-record seam; see guarded.h).
void pipe_enter_first(Task_control_block* blk, Task_ptr* record = nullptr);

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
// several awaiters, N waiters) share one immutable-after-settle result; `take()` is the
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
    // Pipe tasks dispatch through the same trampoline at generation 0 (their blocks are
    // never `reset`), so the slot's meaning is uniform.
    std::atomic<std::uint64_t> dispatch_arg{ 0 };
    // The task's pipe entries (docs/pipe-rebase.md §0.2): an array of `pipe_count` links
    // embedded in the owning allocation (`Piped_executable` or the graph's per-node slab),
    // in canonical (ascending pipe-address) order. Null / 0 for a non-pipe task.
    // `pipe_count` doubles as the `release()` trigger threshold (pipes are entered when it
    // is the only remaining lock count -- pipes-entered-last); `pipes_entered` is the
    // cascade's progress, and settle advances exactly links `[0, pipes_entered)`. Both
    // byte fields are written single-threaded (creation / the sequential cascade) and
    // published by the atomics around them.
    Pipe_link* pipe_links = nullptr;
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
    // state): a dependent (a graph successor) inherits the cancellation, so
    // `Executable::run` cancels instead of running the body. Harmless if set on a task
    // already executing (a cancelled nested child): the flag is only read at execution
    // start. Reset by `reset()` / graph re-arm.
    std::atomic<bool> prereq_cancelled{ false };
    bool completed = false;            // mutex-only
    bool cancelled = false;            // mutex-only
    // Pipe fields (see `pipe_links`): entry count / trigger threshold, and cascade progress.
    std::uint8_t pipe_count = 0;
    std::uint8_t pipes_entered = 0;
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
    // `prereq_cancelled` carries whether the settling prerequisite was *cancelled*: the
    // dependent (a graph successor) then cancels instead of running (checked at execution
    // start). Non-prerequisite releases pass false. Nesting is ordering-only, so a
    // cancelled nested child sets the flag harmlessly (the parent is already executing;
    // the flag is only read before the body).
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
            dispatch_ready(blk, gen);     // pre-execution: all locks (incl. pipe turns) met
        else if (now == execution_flag)
            blk->complete();              // post-execution: nested done -> complete (`gen` unused)
        else if (now == blk->pipe_count)
            pipe_enter_first(blk.get());  // only pipe locks left -> enter line 0 (§5.5 pipes last)
        // The pipe branch is exact: pre-execution counts decrease monotonically (the
        // prerequisite set is frozen at launch), so `pipe_count` is crossed once, by exactly
        // one releaser (`fetch_sub` values are unique); a `pipe_count == 0` task takes the
        // `now == 0` dispatch instead, and post-execution counts sit above `execution_flag`,
        // far from any `pipe_count`.
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
    // (async accesses, externally-triggered `Signal`s) are left to complete on their own.
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

    // Retract what we can, then wait; defined after `current_task` below (the in-task
    // blocking fatal reads it).
    static void retract_or_wait(const Task_ptr& blk);
};

// `Task_ptr` refcount ops (block is complete here). `dec` at 0 runs the wrapper's `destroy`.
inline void intrusive_inc(Task_control_block* p) noexcept
{
    p->refcount.fetch_add(1, std::memory_order_relaxed);
}
// Bounded destruction trampoline. A block's destruction can release references to other
// blocks (a fused coroutine frame owns its `Task` parameters and promise state; a builder
// chain's blocks reference their prerequisites), so a deep chain destroyed recursively --
// dec -> destroy -> member dec -> destroy -> ... -- overflows the stack (a 50k-deep await
// cascade did, under TSan). The last release pushes instead, and the outermost drain
// destroys iteratively (O(1) stack); the vector retains capacity, so the steady state
// allocates nothing.
// Trivially-destructible on purpose: releases can run during thread/process teardown
// (scheduler drain, TLS destructors), after a non-trivial thread-local's destructor would
// already have run -- a `std::vector` here crashed at exit under TSan. The small buffer is
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
        return;   // the active drain on this thread destroys it -- don't recurse
    q.draining = true;
    for (std::size_t head = 0; head < q.size; ++head)
        q.items[head]->destroy(q.items[head]);   // may push more
    q.size = 0;   // buffer retained
    q.draining = false;
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

// A bare block (no result, no body -- `Signal`): one allocation, destroyed as a plain
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
// (it settled before the work it reports on) -- callers that need ordering must go through
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

// The running segment's implicit-scope child list (docs/coroutine-first.md §4.3): a
// coroutine frame installs its own per-frame list around each segment (see
// `coroutine_support.h`), so `ts::nested` can record children for a mid-body
// `co_await ts::join_nested()`. Null for functor bodies (no await, counter-gated only)
// and outside tasks.
inline thread_local std::vector<Task_ptr>* current_scope_children = nullptr;

inline void Task_control_block::retract_or_wait(const Task_ptr& blk)
{
    retract(blk);
    // Worker-less mode: the awaited work (an async access, a released successor) may be
    // queued on THIS thread's serial trampoline behind the current frame -- run it
    // before parking, or nothing ever would. No-op otherwise.
    drain_serial_pending();
#if TS_SAFETY_CHECKS
    // Coroutine-first (docs/coroutine-first.md §4.1): about to genuinely park INSIDE a
    // task (retraction and the serial drain could not finish the target) on work the
    // waiter cannot help along -- fatal, not a warning: the park occupies a worker and
    // risks pool-exhaustion deadlock, and `co_await` is the sanctioned wait. Retractable
    // targets are exempt UNTIL the retraction machinery is removed (stage 4) -- bare-task
    // joins still retract above; `parallel_for` joins never route here (they wait on the
    // group state directly, on provably running helpers). `ready` is an approximation
    // (the target may complete concurrently after the check) -- a rare borderline fatal
    // on a genuinely-parking call, never a missed hazard class.
    if (current_task && !blk->flags.retractable
        && !blk->ready.load(std::memory_order_acquire))
        blocking_sync_diagnose(blk.get());
#endif
    blk->wait();
}

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

// Dependent-false for a `static_assert` in a discarded `if constexpr` branch (the
// plain-`false` form is C++23 but not yet uniform across the toolchains we build on).
template<typename...> inline constexpr bool always_false = false;

// A bare task body (`ts::task`/`launch`/`nested`): no parameters, or a single trailing
// `Cancellation_token` (cooperative cancellation, see `takes_token_v`). Gated at the
// entry points so a wrong shape rejects at the call site naming this concept instead
// of hard-erroring inside `Task_result`. (For a *generic* wrong body the token-arity
// probe still instantiates the body -- same rendering as before the gate; only
// introspectable functors gain the clean rejection.)
template<typename Fn>
concept Task_body = std::invocable<std::decay_t<Fn>&>
    || std::invocable<std::decay_t<Fn>&, const Cancellation_token&>;

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
    {
        return [fn = std::forward<Fn>(fn), ctx = snapshot_access(), owner = trace_owner()](const Cancellation_token& tok) mutable -> R
        {
            Trace_owner_scope trace_owner_scope(owner);
            Trace_busy_scope trace_busy_scope;
            Inherited_access_scope scope(ctx);
            return fn(tok);
        };
    }
    else
    {
        return [fn = std::forward<Fn>(fn), ctx = snapshot_access(), owner = trace_owner()]() mutable -> R
        {
            Trace_owner_scope trace_owner_scope(owner);
            Trace_busy_scope trace_busy_scope;
            Inherited_access_scope scope(ctx);
            return fn();
        };
    }
}

// The control block behind a `Task` handle (used to wire prerequisites).
template<typename R>
Task_ptr core_of(const Task<R>& t) noexcept;

// `core_of`'s inverse: wrap an existing block in a handle. For detail-layer producers
// (`Deferred::commit`'s pre-settled sentinel) that hand out a `Task` for a block they
// did not create through the public builders.
template<typename R>
Task<R> task_from_core(Task_ptr core) noexcept;

} // namespace detail

// Options for a `Guarded` access (`access` / `async`, single- and multi-object). Deliberately
// WITHOUT a run-inline knob: the verb chooses inline-vs-enqueued (`access` runs inline when the
// queue is free via `pipe_try_inline`; `async` always enqueues). `token` makes the body skippable
// before it runs (and is forwarded to a trailing-`Cancellation_token` body for a mid-run
// early-out); `priority` sets the queue position when enqueued.
struct Access_options
{
    Cancellation_token token = {};
    Priority priority = Priority::normal;
};

// Dispatch options for launching a standalone task (`ts::launch`) or a nested one
// (`ts::nested`). `token` makes it skippable before it runs; `priority` sets its queue
// position.
struct Launch_options
{
    Cancellation_token token = {};
    Priority priority = Priority::normal;
};

// Handle to an async result. `co_await` it from a coroutine task (the sanctioned
// composition — see coroutine_support.h); `sync()` blocks for the result from a blue
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
    // `void` for a void one). The reference is valid while a handle (this `Task`, or the
    // `Task_builder` / another copy) keeps the block alive; `T r = t.sync()` copies within the
    // full-expression and is always safe. To *move* the result out (ownership handoff, or a
    // move-only `R`) use `take()`. For a value task, fatal if it was cancelled (no result) —
    // check `is_cancelled()` first; a cancelled `void` sync() simply returns.
    // NOTE: `sync()` waits for THIS task to settle, not for work attached downstream —
    // `settle()` fires internal continuations AFTER waking waiters (`notify_all`), so an
    // attached callback may still be running (or not yet started) when `sync()` returns.
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

// Launch a standalone task on the scheduler — a bare functor with no access target (the
// primitive `async` for work that touches no guarded object). Returns a `Task<R>`; a
// `Launch_options{token, priority}` makes it skippable before it runs and sets its queue
// position. Dispatches through the `submit_ready` bridge (so this stays scheduler-
// independent) and inherits the launcher's access grant, so sub-work launched from a task
// body may touch the launcher's data.
// (Deduced return -- `Task<Task_result_t<Fn>>` -- rather than a trailing return type:
// the trailing form substitutes during overload resolution, BEFORE the constraint is
// checked, so a wrong body shape would hard-error inside `Task_result` instead of
// failing the `Task_body` gate.)
template<typename Fn>
    requires detail::Task_body<Fn>
auto launch(Fn&& fn, Launch_options opts = {})
{
    using R = detail::Task_result_t<Fn>;
    auto core = detail::make_executable<R>(detail::with_inherited_access<R>(std::forward<Fn>(fn)), std::move(opts.token));
    core->flags.retractable = true;   // bare scheduler task (no pipe binding): safe to run inline from a waiter
    core->flags.priority = opts.priority;
    detail::submit_ready(core, core->generation());   // fresh block, pre-dispatch: gen 0, race-free read
    return Task<R>(core);
}

namespace detail
{

// Attach `child` as a NESTED task of the currently-executing task: that task will not
// complete until `child` settles (completed or cancelled). Fatal if there is no running
// task. Detail-level -- the public spellings are `ts::nested` (launch + attach) and the
// graph's coroutine-node wiring; nesting is a completion dependency, orthogonal to how
// the child runs.
inline void add_nested(Task_ptr child_core)
{
    Task_ptr parent = current_task;
    if (!parent)
        ts::fatal("ts::nested called outside a running task");

    parent->num_locks.fetch_add(1, std::memory_order_relaxed);   // a completion lock on the parent

    // Record for a mid-body `co_await ts::join_nested()` when the caller's segment carries an
    // implicit scope (a coroutine frame installs one; functor bodies have none -- they cannot
    // await, and their children gate completion via the counter alone).
    if (current_scope_children != nullptr)
        current_scope_children->push_back(child_core);
    {
        std::scoped_lock lock(child_core->mutex);
        if (!child_core->completed)
        {
            child_core->successors.push_back(std::move(parent));   // child releases parent when it settles
            return;
        }
    }
    Task_control_block::release(parent);   // child already settled -> release the lock now
}

} // namespace detail

// Launch a task and attach it as a nested task of the currently-executing task (its
// completion gates the parent's): the parent will not complete until the child settles.
// Call from within a task body; fatal outside one.
template<typename Fn>
    requires detail::Task_body<Fn>
auto nested(Fn&& fn, Launch_options opts = {})
{
    auto t = launch(std::forward<Fn>(fn), std::move(opts));
    detail::add_nested(detail::core_of(t));
    return t;
}

// A manually-completed synchronization point: a bodyless `Task<void>` (no work is
// scheduled or executed) that you `trigger()` by hand. It is both producer and
// consumer in one handle — the consumer side is inherited from `Task<void>`
// (`co_await` / `sync`, `is_done`), the producer side is `trigger()`. Copyable;
// copies share one control block. Used as a done-signal, a barrier / pipeline-phase
// gate, or an inter-task signal (the integrated equivalent of a manual-reset event
// / a promise+future fused). `trigger()` is idempotent (first call wins), so it is
// safe to trigger from multiple threads or more than once.
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
