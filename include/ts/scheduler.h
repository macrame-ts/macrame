#pragma once

#include "ts/detail/event_count.h"
#include "ts/detail/mpmc_queue.h"
#include "ts/priority.h"
#include "ts/detail/work_stealing_deque.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// Profiling instrumentation (canonical default; `static_task_graph.h` repeats the same
// idempotent block). With it on, each worker accumulates per-task busy time (two clock
// reads per task) feeding `Scheduler::busy_ticks`; define TS_PROFILING=0 in shipping
// builds to compile the counters out.
#ifndef TS_PROFILING
#define TS_PROFILING 1
#endif

#if TS_PROFILING
#include <chrono>
#endif

#include "ts/detail/trace_owner.h"   // scheduler-free seam; arm/disarm bump its bridge flag

namespace ts
{

// How an idle worker (one that found no work) waits for more. Chosen per `Scheduler` instance
// at construction -- a runtime enum, not a compile-time switch: the only hot-path read is one
// branch on the const `idle_policy_` member in `submit` (perfectly predicted) plus, in
// `handoff`, one relaxed load -- no measurable cost, so a runtime knob buys per-instance
// flexibility for free. See `worker_thread.cpp` (consumer side) and `signal_submit` (producer).
enum class Idle_policy
{
    // Never park: every idle worker yields in a loop forever. Lowest wake latency, but burns a
    // core per idle worker. Producers never signal.
    spin,
    // UE-style (the default): an idle worker spins `spin_cycles` scans before parking on the
    // eventcount, so a task arriving during the spin window is grabbed with no wake latency.
    // Producers wake one parked worker on every submit (spinning workers grab work without the
    // wake). `spin_cycles == 0` degenerates to park-immediately.
    spin_then_block,
    // Go-style spinner handoff: at most a few workers spin; a spinner that finds work relinquishes
    // the spinner role and, if it was the last spinner, wakes a successor to spin BEFORE it runs
    // -- so the pool keeps discovering work without the producer waking anyone. Producers issue
    // the wake syscall ONLY when no spinner exists (the fully-parked 0->1 transition); otherwise
    // a submit is a cheap epoch bump. Moves the wake cost off the producer onto the consumer.
    handoff,
};

struct Scheduler_config
{
    uint32_t num_threads = 0;                    // 0 -> `std::thread::hardware_concurrency()`
    Idle_policy idle_policy = Idle_policy::spin_then_block;
    // `spin_then_block`/`handoff`: number of `find_work` scans an idle worker spins before it
    // parks (UE's `WorkerSpinCycles` is ~53). Ignored by `spin`.
    uint32_t spin_cycles = 64;
    // Worker-less mode (UE's no-multithreading shape): no worker threads exist, and every
    // submitted task executes INLINE at its submit point, on the submitting thread, via a
    // bounded FIFO trampoline (a chain of tasks drains iteratively, not recursively). Serves
    // deterministic debugging/tests, thread-less platforms, and low-end fallback. Semantics
    // to be aware of: a task body runs BEFORE `launch`/`async` returns (reentrancy is
    // observable -- launching under a user lock runs the body under that lock); priorities
    // and the idle policy are no-ops; work triggered from an external thread runs on that
    // thread. `num_threads` is ignored when set.
    bool single_threaded = false;
};

// The library runs one process-wide scheduler (`global_scheduler()`, declared in guarded.h).
// It is RECONFIGURABLE: `configure_scheduler` tears the current one down (signals quit, JOINs
// its workers, drains) and builds a new one with `config`. This is a coarse lifecycle
// operation for quiescent points (startup, between frames/phases) -- it is NOT thread-safe
// against concurrent use: calling it while tasks are in flight from other threads is
// undefined. `current_scheduler_config` returns the config currently in effect.
void configure_scheduler(Scheduler_config config);
Scheduler_config current_scheduler_config();

// RAII: reconfigure the global scheduler for a scope, restoring the previous config on exit.
// The single-global way to run a block on a specific pool (e.g. a sample tracing on a fixed
// worker count) -- there are no ad-hoc `Scheduler` instances to construct. Same coarse
// teardown+recreate semantics as `configure_scheduler`; use at quiescent points only.
class Scheduler_scope
{
public:
    explicit Scheduler_scope(Scheduler_config config)
        : prev_(current_scheduler_config())
    {
        configure_scheduler(config);
    }
    ~Scheduler_scope() { configure_scheduler(prev_); }

    Scheduler_scope(const Scheduler_scope&) = delete;
    Scheduler_scope& operator=(const Scheduler_scope&) = delete;

private:
    Scheduler_config prev_;
};

using Task_func_ptr = void(*)(void* data);

namespace detail
{

// A queued task: the func + its data. Priority is not a field -- it is the queue the
// task lives in (one lock-free MPMC queue per priority, scanned high->low). MUST stay 16 bytes
// (two words): the work-stealing deque stores cells as `std::atomic<Task_entry>`, lock-free only
// while the element is double-word-CAS-able. A block dispatch fits here as `{trampoline, block}` --
// the block IS the payload (it carries its own body, priority, and claim), so nothing else rides
// in this entry.
struct Task_entry
{
    Task_func_ptr func_ = nullptr;
    void* data_ = nullptr;
};

class Worker_thread;

// One queue per `Priority` value (high/normal/low), indexed by the enum.
inline constexpr std::size_t priority_count = 3;

} // namespace detail

class Scheduler;
extern thread_local Scheduler* current_scheduler;
// This thread's worker index within `current_scheduler` (>= 0 for a worker of it, else -1).
// Routes a worker's own `normal` submits to its local deque, and identifies it in stealing.
extern thread_local int current_worker_index;

class Scheduler
{
    friend class detail::Worker_thread;

public:
    Scheduler(Scheduler_config config = {});
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void submit(Task_func_ptr func, void* data, Priority priority = Priority::normal);

    // Number of worker threads (the natural default concurrency for `parallel_for`).
    // Defined in the .cpp -- `Worker_thread` is incomplete here. 0 in worker-less mode.
    int worker_count() const noexcept;

    // Worker-less (single-threaded) scheduler: no workers, tasks execute inline at submit.
    bool single_threaded() const noexcept { return single_threaded_; }

    // Quiescence: every worker is sitting in the idle path and every queue is empty, so this
    // scheduler cannot make progress on its own. Racy by nature (a worker may be between
    // finding work and marking itself busy), which is why the deadlock net requires the
    // condition to hold CONTINUOUSLY for a window rather than trusting one sample. Feeds
    // `detail::scheduler_quiescent`, the seam the task layer reads (`Rule::deadlock_net`).
    //
    // Worker idleness is an explicit transition count, not "parked": under `Idle_policy::spin`
    // a worker never parks, so a park-based test would never report idle. `worker_idle` /
    // `worker_busy` fire only when a worker crosses between finding work and not finding any,
    // so a spinning worker marks itself idle once and stays that way.
    bool quiescent() const noexcept
    {
        return idle_workers_.load(std::memory_order_acquire) >= worker_count() && all_empty();
    }

    void worker_idle() noexcept { idle_workers_.fetch_add(1, std::memory_order_acq_rel); }
    void worker_busy() noexcept { idle_workers_.fetch_sub(1, std::memory_order_acq_rel); }

#if TS_PROFILING
    // Total wall time (raw `steady_clock` ticks) this scheduler's workers have spent
    // executing tasks -- every task kind (graph nodes, `parallel_for` slices, async pipe
    // jobs, continuations). Feeds the trace's core-utilization metric: the busy delta over
    // a run window, divided by `workers * window`. Work run inline on non-worker threads
    // (inline roots on the caller) is not counted. Relaxed sum over per-worker
    // counters -- a snapshot, exact only at quiescent points.
    long long busy_ticks() const noexcept
    {
        // Completed spans plus the elapsed part of each in-flight task: the reader is
        // often ITSELF inside a task (a graph run's fold happens on the settling worker),
        // so counting only completed spans would miss the whole current task -- on a
        // one-worker scheduler that is the entire window. Relaxed snapshot; a reader
        // interleaving a task's completion can transiently double-count that one span
        // (advisory metric, not an invariant).
        long long now = std::chrono::steady_clock::now().time_since_epoch().count();
        long long sum = 0;
        for (const Busy_slot& slot : busy_)
        {
            sum += slot.ticks.load(std::memory_order_relaxed);
            long long started = slot.started.load(std::memory_order_relaxed);
            if (started != 0 && now > started)
                sum += now - started;
        }
        return sum;
    }

    // Total tasks run across all workers while armed (every kind: graph node bodies,
    // parallel_for slices, async pipe jobs, continuations). Monotonic like `busy_ticks`;
    // the trace takes a begin/end delta per run. Relaxed sum -- advisory.
    long long task_count() const noexcept
    {
        long long sum = 0;
        for (const Busy_slot& slot : busy_)
            sum += slot.tasks.load(std::memory_order_relaxed);
        return sum;
    }

    // Task-system-cost accumulators (ticks, summed over workers; relaxed -- advisory). `body`
    // = user-functor time, `machinery` = scheduler overhead. The trace takes begin/end deltas
    // per run; overhead = machinery / (body + machinery). See `Busy_slot`.
    long long body_ticks() const noexcept
    {
        long long sum = 0;
        for (const Busy_slot& slot : busy_)
            sum += slot.body.load(std::memory_order_relaxed);
        return sum;
    }
    // Total orchestration ticks (the dedicated per-run `execute()` setup accumulator, Phase 1
    // of the four-way breakdown), summed over workers; relaxed -- advisory. No in-flight
    // compensation: the setup span is booked whole at `Trace_setup_scope` exit, never a
    // partially-elapsed in-flight task like a `run_task` body. The trace takes a begin/end
    // delta per run, exactly like `body_ticks`/`machinery_ticks`.
    long long orchestration_ticks() const noexcept
    {
        long long sum = 0;
        for (const Busy_slot& slot : busy_)
            sum += slot.orchestration.load(std::memory_order_relaxed);
        return sum;
    }
    long long machinery_ticks() const noexcept
    {
        // Mirror `busy_ticks`' in-flight handling, for the same reason: the reader is often
        // the settling worker, INSIDE its terminal task. That task's body scope has already
        // netted its span OUT of machinery (M -= F), but `run_task` has not yet booked the
        // whole span back IN (M += R happens after `func_` returns) -- so a plain sum reads
        // low by the settling body. Add each in-flight task's elapsed span (its pending
        // `run_task` machinery), which compensates the already-subtracted body. Relaxed
        // snapshot; advisory (a reader interleaving a completion can transiently over/under).
        long long now = std::chrono::steady_clock::now().time_since_epoch().count();
        long long sum = 0;
        for (const Busy_slot& slot : busy_)
        {
            sum += slot.machinery.load(std::memory_order_relaxed);
            long long started = slot.started.load(std::memory_order_relaxed);
            if (started != 0 && now > started)
                sum += now - started;
        }
        return sum;
    }

    // Add `dt` ticks of dispatch machinery (a successful `find_work` scan) to `worker_index`'s
    // slot -- called from the worker loop, armed-only. Single writer (this worker).
    void add_dispatch_ticks(int worker_index, long long dt) noexcept
    {
        Busy_slot& slot = busy_[static_cast<size_t>(worker_index)];
        slot.machinery.fetch_add(dt, std::memory_order_relaxed);
    }
    // Move `dt` ticks of a worker's time from machinery to body (a user functor ran) -- the
    // `trace_body_add` bridge target. Single writer per slot. A non-worker caller lands in the
    // overflow lane (the last slot), so an inline body run on the main thread still nets.
    void add_body_ticks(int worker_index, long long dt) noexcept
    {
        Busy_slot& slot = busy_[busy_slot_index(worker_index)];
        slot.body.fetch_add(dt, std::memory_order_relaxed);
        slot.machinery.fetch_add(-dt, std::memory_order_relaxed);
    }
    // Credit `dt` ticks to body without netting machinery -- an inline-dispatched body that
    // did not pass through `run_task` (nothing booked its span as machinery). A non-worker
    // caller (worker-less mode's inline drain, an external submit) lands in the overflow lane;
    // this is what feeds body (B) for the worker-less ground-truth overhead.
    void add_body_only(int worker_index, long long dt) noexcept
    {
        busy_[busy_slot_index(worker_index)].body.fetch_add(dt, std::memory_order_relaxed);
    }
    // Add `dt` ticks of machinery -- the `trace_machinery_add` bridge target (a graph run's
    // per-run setup span, folded into M). A non-worker caller lands in the overflow lane.
    void add_machinery_ticks(int worker_index, long long dt) noexcept
    {
        busy_[busy_slot_index(worker_index)].machinery.fetch_add(dt, std::memory_order_relaxed);
    }
    // Add `dt` ticks of ORCHESTRATION -- the `trace_orchestration_add` bridge target (Phase 1
    // of the four-way subtraction breakdown). Same per-run graph-setup span the machinery
    // bridge sees, booked to a DEDICATED accumulator so the four-way split has a clean
    // off-worker bucket while summed-M stays whole. A non-worker caller lands in the overflow
    // lane, exactly like `add_machinery_ticks`.
    void add_orchestration_ticks(int worker_index, long long dt) noexcept
    {
        busy_[busy_slot_index(worker_index)].orchestration.fetch_add(dt, std::memory_order_relaxed);
    }
    // Move `dt` ticks of a worker's time from body to machinery (a `submit` ran inside a
    // functor -- fan-out dispatch is task-system cost, not user work). No-op off-worker.
    void add_submit_ticks(int worker_index, long long dt) noexcept
    {
        if (worker_index < 0)
            return;
        Busy_slot& slot = busy_[static_cast<size_t>(worker_index)];
        slot.machinery.fetch_add(dt, std::memory_order_relaxed);
        slot.body.fetch_add(-dt, std::memory_order_relaxed);
    }

    // Time buckets the utilization background samples each run into.
    static constexpr int util_bucket_count = 128;

    // Arm/disarm busy tracking (counted -- concurrent consumers nest). Armed by a traced
    // graph run for its window; work already in flight when arming is not back-stamped
    // (an under-count of at most one task span at the window edge; advisory metric).
    // `origin` = the run's begin tick, `bucket_width` = the tick width of each utilization
    // bucket (0 disables per-bucket accumulation for this window -- e.g. the first run,
    // before a makespan estimate exists). Arming clears the per-worker bucket rows.
    void arm_busy_tracking(long long origin, long long bucket_width, int owner_count = 0) noexcept
    {
        bucket_origin_.store(origin, std::memory_order_relaxed);
        bucket_width_.store(bucket_width, std::memory_order_relaxed);
        if (bucket_width > 0)
        {
            for (Bucket_row& row : bucket_busy_)
            {
                for (std::atomic<long long>& b : row.b)
                    b.store(0, std::memory_order_relaxed);
            }
        }
        // Per-owner (graph-node) busy sink: sized + cleared per armed run (node counts can
        // differ between compiles). Written relaxed by every worker via `add_owner_busy`.
        if (static_cast<int>(owner_busy_.size()) != owner_count)
            owner_busy_ = std::vector<std::atomic<long long>>(static_cast<std::size_t>(owner_count));
        for (std::atomic<long long>& o : owner_busy_)
            o.store(0, std::memory_order_relaxed);
        busy_tracking_.fetch_add(1, std::memory_order_relaxed);
        detail::trace_owner_armed.fetch_add(1, std::memory_order_relaxed);   // scheduler-free owner seam
    }
    void disarm_busy_tracking() noexcept
    {
        detail::trace_owner_armed.fetch_sub(1, std::memory_order_relaxed);
        busy_tracking_.fetch_sub(1, std::memory_order_relaxed);
    }

    bool busy_armed() const noexcept { return busy_tracking_.load(std::memory_order_relaxed) != 0; }

    long long bucket_width() const noexcept { return bucket_width_.load(std::memory_order_relaxed); }

    // Attribute `dt` ticks of busy time to graph node `owner` (its body / slices / async).
    // Called from `detail::Trace_busy_scope` while armed; relaxed, contended only on a hot
    // node's own slot (profiling-only).
    void add_owner_busy(int owner, long long dt) noexcept
    {
        if (owner >= 0 && owner < static_cast<int>(owner_busy_.size()))
            owner_busy_[static_cast<std::size_t>(owner)].fetch_add(dt, std::memory_order_relaxed);
    }

    // Copy the per-node busy into `out[0..count)` (ticks), at the fold after the run's
    // completion barrier published the workers' writes (relaxed, like `read_bucket_busy`).
    void read_owner_busy(long long* out, int count) const noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            out[i] = i < static_cast<int>(owner_busy_.size())
                ? owner_busy_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed) : 0;
        }
    }

    // Sum the per-worker bucket busy into `out[0..util_bucket_count)` (ticks). Read at the
    // fold on the settling thread, after the run's completion barrier has published the
    // workers' writes (relaxed loads -- advisory, like `busy_ticks`).
    void read_bucket_busy(long long* out) const noexcept
    {
        for (int b = 0; b < util_bucket_count; ++b)
            out[b] = 0;
        for (const Bucket_row& row : bucket_busy_)
        {
            for (int b = 0; b < util_bucket_count; ++b)
                out[b] += row.b[static_cast<std::size_t>(b)].load(std::memory_order_relaxed);
        }
    }
#endif

private:
    // Find one task for worker `worker_index`, scanning: global high -> its own local deque
    // (LIFO, cache-hot) -> global normal -> global low -> steal `normal` from a random victim.
    // High stays strict (checked first). True if a task was found.
    bool find_work(int worker_index, detail::Task_entry& out);
    // `find_work`, timing a SUCCESSFUL scan into dispatch machinery (armed-only). A scan that
    // finds nothing is idle (lack of parallelism), not scheduling cost, so it is not charged.
    bool find_work_dispatch(int worker_index, detail::Task_entry& out)
    {
#if TS_PROFILING
        if (busy_tracking_.load(std::memory_order_relaxed) != 0)
        {
            long long t0 = std::chrono::steady_clock::now().time_since_epoch().count();
            bool got = find_work(worker_index, out);
            if (got)
                add_dispatch_ticks(worker_index, std::chrono::steady_clock::now().time_since_epoch().count() - t0);
            return got;
        }
#endif
        return find_work(worker_index, out);
    }
#if TS_PROFILING
    // Map a worker index to its busy slot: a real worker gets its own slot; a non-worker
    // caller (`worker_index < 0`) gets the trailing overflow lane (`busy_.size() - 1`), which
    // always exists (the ctor sizes `busy_` to worker_count + 1, so even worker-less mode has
    // one slot).
    std::size_t busy_slot_index(int worker_index) const noexcept
    {
        return worker_index >= 0 ? static_cast<std::size_t>(worker_index) : busy_.size() - 1;
    }
#endif

    // Approximate: all global queues AND all local deques empty (racy; shutdown-drain check).
    bool all_empty() const;

    // Producer-side signal after a submit, per `idle_policy_` (see the enum).
    void signal_submit();

    // Worker-less mode: run `{func, data}` inline on the calling thread via the per-thread
    // FIFO trampoline (see `serial_pending` in the .cpp). Installs this scheduler as the
    // thread's current scheduler for the drain (pipe job re-dispatch and nested submits
    // resolve to it), so external threads submitting into a worker-less scheduler work.
    void run_serial(Task_func_ptr func, void* data);

    // Idle behavior for the configured policy, called by the worker when `find_work` came up
    // empty. Returns true with a task in `out` to run; false means "re-scan / re-check quit and
    // loop" (it parked and was woken, or it spin-yielded). The `handoff` path also maintains
    // `num_spinning_` and promotes a successor spinner.
    bool wait_for_work(int worker_index, detail::Task_entry& out);
    // prepare/recheck/commit park on the eventcount (used by every parking policy).
    bool park(int worker_index, detail::Task_entry& out);
    // Spin up to `cycles` `find_work` scans, relaxing between; true with a task in `out` on the
    // first hit, false if it spun out empty. Bails on quit.
    bool spin_scan(int worker_index, detail::Task_entry& out, std::uint32_t cycles);
    // The `handoff` spinner protocol (become a spinner, hand off on find, else park).
    bool handoff_wait(int worker_index, detail::Task_entry& out);

    // Execute one task on worker `worker_index`, accumulating its wall time into the
    // worker's busy counter (single writer per slot, cacheline-padded -- no contention;
    // relaxed, read by `busy_ticks` cross-thread). A plain call without TS_PROFILING.
    void run_task(int worker_index, const detail::Task_entry& task)
    {
#if TS_PROFILING
        // Timed only while a consumer is armed (a traced graph run in flight): the two
        // clock reads cost ~tens of ns, visible on empty-task throughput, so idle
        // tracking must not tax untraced work. Disarmed cost: one relaxed load + branch.
        if (busy_tracking_.load(std::memory_order_relaxed) != 0)
        {
            Busy_slot& slot = busy_[static_cast<size_t>(worker_index)];
            slot.tasks.fetch_add(1, std::memory_order_relaxed);   // task-volume counter (every kind)
            long long t0 = std::chrono::steady_clock::now().time_since_epoch().count();
            slot.started.store(t0, std::memory_order_relaxed);   // in-flight, for busy_ticks
            // The whole span is booked as machinery below; flag the body it dispatches so its
            // `Trace_busy_scope` nets its own span back into body (inline bodies clear this).
            detail::trace_body_under_run_task = true;
            task.func_(task.data_);
            detail::trace_body_under_run_task = false;
            long long t1 = std::chrono::steady_clock::now().time_since_epoch().count();
            slot.ticks.store(slot.ticks.load(std::memory_order_relaxed) + (t1 - t0),
                             std::memory_order_relaxed);
            // Whole task span defaults to machinery; the functor bracket inside `func_`
            // already moved its own span to `body` (M -= F, B += F via `trace_body_add`),
            // so this task's net machinery is (setup + completion + successor submit).
            slot.machinery.fetch_add(t1 - t0, std::memory_order_relaxed);
            slot.started.store(0, std::memory_order_relaxed);
            // Distribute the task's [t0,t1] busy span across the utilization time buckets it
            // spans (relative to the run origin). Armed-only; most tasks touch one bucket.
            long long bw = bucket_width_.load(std::memory_order_relaxed);
            if (bw > 0)
            {
                long long origin = bucket_origin_.load(std::memory_order_relaxed);
                long long lo = t0 - origin, hi = t1 - origin;
                long long cap = static_cast<long long>(util_bucket_count) * bw;
                if (hi > 0 && lo < cap)
                {
                    if (lo < 0) lo = 0;
                    if (hi > cap) hi = cap;
                    int b0 = static_cast<int>(lo / bw), b1 = static_cast<int>((hi - 1) / bw);
                    Bucket_row& row = bucket_busy_[static_cast<std::size_t>(worker_index)];
                    for (int b = b0; b <= b1 && b < util_bucket_count; ++b)
                    {
                        long long edge_lo = static_cast<long long>(b) * bw;
                        long long edge_hi = edge_lo + bw;
                        long long seg_lo = lo > edge_lo ? lo : edge_lo;
                        long long seg_hi = hi < edge_hi ? hi : edge_hi;
                        if (seg_hi > seg_lo)
                            row.b[static_cast<std::size_t>(b)].fetch_add(seg_hi - seg_lo, std::memory_order_relaxed);
                    }
                }
            }
            return;
        }
#endif
        task.func_(task.data_);
    }

    // One lock-free MPMC queue per priority (index = the `Priority` enum value). `high`/`low`
    // are global-only; `normal` also has per-worker deques (below), with this as overflow +
    // the injector for external (non-worker) submits.
    std::array<detail::Mpmc_queue<detail::Task_entry>, detail::priority_count> queues_;
    // Per-worker Chase-Lev deque for `normal`: a worker's own `normal` submits go here (LIFO,
    // no shared cache line -- the producer fast path); thieves steal FIFO. One per worker.
    std::vector<std::unique_ptr<detail::Work_stealing_deque<detail::Task_entry>>> local_normal_;
    std::atomic<bool> quit_ = false;
    detail::Event_count events_;   // wakes idle (parking) workers
    // `handoff` policy: advisory count of workers currently spinning. Correctness rides the
    // always-advanced epoch (see `signal_submit`), so this can be read/written relaxed -- it
    // only gates the wake syscall and the successor-promotion decision.
    std::atomic<int> num_spinning_ = 0;
    // Workers currently in the idle path (see `quiescent`). Written only at the
    // busy<->idle transitions, never per task, so the hot path is untouched.
    std::atomic<int> idle_workers_ = 0;
    const Idle_policy idle_policy_;
    const std::uint32_t spin_cycles_;
    const bool single_threaded_;
    std::vector<detail::Worker_thread> workers_;
#if TS_PROFILING
    // Per-worker busy-time counters, one padded slot each (single writer: the owning
    // worker in `run_task`; `busy_ticks` reads them relaxed). `ticks` = completed task
    // spans; `started` = the in-flight task's start tick (0 = idle). Sized in the ctor
    // BEFORE the workers start. Declared last: the hot members above keep their layout.
    struct alignas(64) Busy_slot
    {
        std::atomic<long long> ticks{ 0 };
        std::atomic<long long> started{ 0 };
        std::atomic<long long> tasks{ 0 };   // count of tasks this worker ran while armed
        // Task-system-cost split (all single-writer = this worker; read at fold). `body` =
        // user-functor time (via `trace_body_add`); `machinery` = scheduler overhead:
        // run_task defaults its whole span here, the functor bracket moves its span out to
        // `body`, submit-from-within-a-body and successful find_work add back in. So
        // machinery = dispatch + completion + successor/fan-out submit + successful scans.
        std::atomic<long long> body{ 0 };
        std::atomic<long long> machinery{ 0 };
        // Phase 1 four-way breakdown: the per-run `execute()` setup span, booked here IN
        // ADDITION to `machinery` (via `add_orchestration_ticks`), so the subtraction split
        // has a dedicated off-worker bucket without perturbing summed-M. Single writer.
        std::atomic<long long> orchestration{ 0 };
    };
    std::vector<Busy_slot> busy_;
    std::atomic<int> busy_tracking_{ 0 };   // armed-consumer count; 0 = untimed fast path
    // Per-worker time-bucketed busy for the true-utilization background (single writer: the
    // owning worker in `run_task`; summed by `read_bucket_busy` at the fold). One padded row
    // per worker; sized in the ctor before workers start.
    struct alignas(64) Bucket_row
    {
        std::atomic<long long> b[util_bucket_count];
    };
    std::vector<Bucket_row> bucket_busy_;
    std::atomic<long long> bucket_origin_{ 0 };   // run-begin tick (bucket frame origin)
    std::atomic<long long> bucket_width_{ 0 };    // tick width per bucket; 0 = not bucketing
    // Per-graph-node busy sink for per-node true-busy (body + slices + async), keyed by node
    // index; written relaxed by any worker via `add_owner_busy`, read at the fold. Sized per
    // armed run (see `arm_busy_tracking`).
    std::vector<std::atomic<long long>> owner_busy_;
#endif
};

} // namespace ts
