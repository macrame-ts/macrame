#include "ts/scheduler.h"
#include "ts/detail/worker_thread.h"

#include <cstddef>
#include <cstdint>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    #include <immintrin.h>   // _mm_pause
#endif

namespace ts
{

#if TS_PROFILING
namespace detail
{
// The owner-attribution bridge (declared scheduler-free in detail/trace_owner.h so task.h
// can use it): a running task's `Trace_busy_scope` routes its busy span here. Every bridge
// resolves the one process-wide `global_scheduler()` - with the single-global collapse there
// is no ambient "current" pool to pick, and each bridge fires only while a traced run is armed
// (see `trace_owner_armed`), so the global pool always exists (no side-effect creation). The
// worker index selects the busy slot: a real worker gets its own, a non-worker caller (the
// frame loop, a test, the worker-less drain - `current_worker_index == -1`) lands in the
// scheduler's overflow lane. Armed count bumped by `arm/disarm_busy_tracking`.
std::atomic<int> trace_owner_armed{ 0 };
void (*trace_owner_add)(int, long long) = +[](int owner, long long dt)
{
    detail::Scheduler_profiling::add_owner_busy(global_scheduler(), owner, dt);
};
// The body bridge: a functor's span is credited to body (B) on the worker that ran it. B is
// add-only - machinery is derived by pure subtraction (`M = busy - B`), not accumulated.
void (*trace_body_add)(long long) = +[](long long dt)
{
    detail::Scheduler_profiling::add_body_ticks(global_scheduler(), current_worker_index(), dt);
};
// The span gate: B is credited only inside a timed `run_task` span (see `Scheduler::in_timed_span`).
bool (*trace_span_timed)() = +[]
{
    return detail::Scheduler_profiling::in_timed_span(global_scheduler(), current_worker_index());
};
// The orchestration bridge (the fourth bucket of the four-way subtraction split): `Trace_setup_scope`
// (in static_task_graph.cpp's execute()) routes the per-run top-level setup+initial-dispatch span
// here - the overflow lane when `execute()` runs on a non-worker thread (the common case: the frame
// loop / a test). This folds the node-count-scaling per-run setup (link binding, node re-arm, indegree
// init, root dispatch), which lives in no `run_task` span and so is absent from `busy`, into a clean
// off-worker bucket. A nested in-body run's setup is not booked here (it is inside the enclosing node's
// span already - see `Trace_setup_scope`).
void (*trace_orchestration_add)(long long) = +[](long long dt)
{
    detail::Scheduler_profiling::add_orchestration_ticks(global_scheduler(), current_worker_index(), dt);
};
}
#endif

namespace
{
// Cheap per-thread xorshift for random steal-victim selection (avoids every idle worker
// hammering the same victim).
std::uint32_t next_rand()
{
    static thread_local std::uint32_t s = 0x9e3779b9u ^ static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&s));
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// A spin-loop pause: hints the CPU to back off (frees the pipeline / lets a hyperthread run)
// without yielding to the OS. Much lighter than `std::this_thread::yield` for a short spin.
inline void cpu_relax() noexcept
{
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}
}

Scheduler::Scheduler(Scheduler_config config)
    : idle_policy_(config.idle_policy)
    , spin_cycles_(config.spin_cycles)
    , single_threaded_(config.single_threaded)
{
    uint32_t num_workers = config.num_workers;
    if (num_workers == 0)
        num_workers = std::thread::hardware_concurrency();
    if (num_workers == 0)
        num_workers = 1;   // hardware_concurrency() may return 0 (unknown); never build a worker-less pool by accident
    if (single_threaded_)
        num_workers = 0;   // worker-less: no deques, no workers; `submit` runs tasks inline

    // Per-worker deques must exist before any worker starts (workers steal from all of them).
    local_normal_.reserve(num_workers);
    for (uint32_t i = 0; i < num_workers; ++i)
        local_normal_.push_back(std::make_unique<detail::Work_stealing_deque<detail::Task_entry>>());

#if TS_PROFILING
    // One busy slot per worker plus a trailing overflow lane (the last slot) for non-worker
    // callers (`current_worker_index < 0`): body/machinery charged from the main thread (a
    // graph `execute()`'s per-run setup) or an external submitter lands here instead of being
    // dropped. Worker-less mode has zero workers, so the overflow lane is the only slot - it
    // is what makes the body accumulator (and the worker-less ground-truth overhead) work with
    // no workers. Buckets are worker-only (run_task, index >= 0), so bucket_busy_ stays sized
    // to the worker count.
    busy_ = std::vector<Busy_slot>(num_workers + 1);   // +1 overflow lane; before any worker starts
    bucket_busy_ = std::vector<Bucket_row>(num_workers);
#endif

    workers_.reserve(num_workers);
    for (uint32_t i = 0; i < num_workers; ++i)
        workers_.emplace_back(*this, static_cast<int>(i));
}

namespace detail
{
// The sanctioned `Scheduler` factory (the ctor is private). A friend, so it can reach the
// private constructor; hands back a `unique_ptr` because `Scheduler` is non-movable.
std::unique_ptr<Scheduler> make_scheduler(Scheduler_config config)
{
    return std::unique_ptr<Scheduler>(new Scheduler(config));
}
}

Scheduler::~Scheduler()
{
    quit_.store(true, std::memory_order_release);

    // wake every parked worker so it observes `quit_` and drains/exits (a not-yet-parked
    // worker sees the bumped epoch in its re-check / commit_wait, so none is missed). Every
    // parking policy needs this; only `spin` never parks.
    if (idle_policy_ != Idle_policy::spin)
        events_.notify_all();

    // Join the workers here, in the destructor body, before any member is torn down.
    // Relying on the `workers_` member destructor to join is unsafe: members destruct in
    // reverse declaration order, and the `TS_PROFILING` counters (`busy_`, `bucket_busy_`,
    // `owner_busy_`) are declared after `workers_` (deliberately - to keep the hot members'
    // layout, see scheduler.h), so they would be freed first. A worker still mid-`run_task`
    // then touches a freed `busy_[worker_index]` -> heap-use-after-free (a ~1% teardown race,
    // TS_PROFILING-only). Joining now guarantees no worker runs during member destruction.
    // (`Worker_thread` holds a `std::jthread` that joins on destruction; `clear()` runs them.)
    workers_.clear();
}

void Scheduler::submit(Task_func_ptr func, void* data, Priority priority)
{
    // Worker-less mode: no queues, no workers - the task executes inline, now, on this
    // thread (UE's zero-worker launch shape). `priority` is a no-op here by design.
    if (single_threaded_)
    {
        run_serial(func, data);
        return;
    }

    // A worker submitting `normal` pushes to its own local deque (LIFO, cache-hot, no shared
    // cache line - the producer fast path). External/non-normal submits, and a full local
    // deque, go to the global queue for the priority. With the single-global collapse, being on
    // a worker (`current_worker_index() >= 0`) means being on this pool's worker - the only pool
    // with workers - so the index is a valid slot; the size check is a cheap belt-and-braces
    // guard (an isolated worker-less instance never reaches here - it returns above).
    const int worker = current_worker_index();
    if (priority == Priority::normal && worker >= 0
        && static_cast<std::size_t>(worker) < local_normal_.size()
        && local_normal_[static_cast<std::size_t>(worker)]->push({ func, data }))
    {
        signal_submit();   // wake a thief to help (no-op if none parked)
        return;
    }

    queues_[static_cast<std::size_t>(priority)].push({ func, data });

    signal_submit();
}

namespace
{
// Worker-less mode's per-thread FIFO trampoline (the same iterative-drain shape as the
// inline-dispatch trampoline in task.h): tasks submitted while a drain is active on this
// thread are appended and picked up in submission order by the active drain, so a chain of
// tasks (A's body launches B, B's body launches C, ...) runs iteratively, never recursively.
// `serial_head` is shared state (not a loop local) so a reentrant drain - a body calling
// `sync()` on work it just admitted, via `drain_serial_pending` - continues from where the
// outer drain stands instead of re-running entries.
//
// These three carry no thread-local barrier (ts/detail/thread_local.h) and need none: they
// have internal linkage in a translation unit that compiles no coroutine, and the only
// functions that touch them are the out-of-line `run_serial`/`drain_serial`/
// `drain_serial_pending` below. No caller's frame can hold their address, so there is nothing
// for a suspension to carry across.
thread_local std::vector<detail::Task_entry> serial_pending;
thread_local std::size_t serial_head = 0;
thread_local bool serial_draining = false;

void drain_serial()
{
    while (serial_head < serial_pending.size())
    {
        detail::Task_entry e = serial_pending[serial_head++];
        e.func_(e.data_);
    }
}
}

void Scheduler::run_serial(Task_func_ptr func, void* data)
{
    serial_pending.push_back({ func, data });
    if (serial_draining)
        return;   // the active drain on this thread runs it, in order

    serial_draining = true;
    // Nested submits resolve the one process-wide `global_scheduler()` (this worker-less pool),
    // so no per-thread scheduler install is needed. `current_worker_index` stays -1 - there
    // are no workers, and the profiling accumulators land in the overflow lane on -1.
    drain_serial();
    serial_pending.clear();   // retains capacity
    serial_head = 0;
    serial_draining = false;
}

namespace detail
{

// Seam for task.h's blocking waits (declared there, scheduler-free): before parking, a
// waiter under a worker-less scheduler drains this thread's pending serial tasks - the
// awaited work may be sitting in the trampoline behind the waiter's own frame (a body that
// admitted work and then `sync()`s it). Reentrant: continues the active drain's shared
// head. No-op when nothing is pending (any scheduler mode).
void drain_serial_pending() noexcept
{
    drain_serial();
}

} // namespace detail

// Producer-side wake, per policy. `block`/`spin_then_block` wake one parked worker every submit;
// `handoff` always advances the epoch (cheap - releases any worker about to park) but issues the
// wake syscall only when no spinner exists to discover the work; `spin` never signals.
void Scheduler::signal_submit()
{
    switch (idle_policy_)
    {
    case Idle_policy::spin:
        break;
    case Idle_policy::spin_then_block:
        events_.notify_one();
        break;
    case Idle_policy::handoff:
        events_.advance();   // move the epoch so a parking worker's commit_wait returns
        if (num_spinning_.load(std::memory_order_relaxed) == 0)
            events_.wake_one();   // no spinner -> pay the wake syscall (0->1 transition)
        break;
    }
}

// Serve a `low` task after this many consecutive high/normal tasks, so a steady stream of
// normal work cannot starve low indefinitely (aging valve, à la Go/Tokio's "check global
// every ~61/31"). Low priority still means low: it just gets guaranteed occasional progress.
constexpr int low_valve_threshold = 64;

// Find one task for `worker_index`: global high (strict) -> own local deque (LIFO) -> global
// normal -> steal `normal` from a random victim -> global low. A per-worker aging counter
// forces `low` ahead of normal once in a while (the starvation valve).
//
// Steal is served before global low: a `normal` task outranks a `low` task even when the
// only normal work left lives in another worker's local deque (reachable solely by steal).
// The alternative - cheap global-low pop before the steal scan - inverts priority: a
// low-priority global task would beat a normal task buried under a LIFO sibling in a local
// deque (measured: `audio` at `low` beating a normal graph successor stuck behind a longer
// sibling). Honouring the normal > low contract costs an O(workers) steal scan on the idle
// path before falling through to a waiting low task; the valve still guarantees low makes
// progress. (Rank-aware successor submission - TODO 2.5 - is the deeper fix: it keeps the
// important normal successor as the LIFO top so it is taken locally, never left steal-only.)
bool Scheduler::find_work(int worker_index, detail::Task_entry& out)
{
    static thread_local int since_low = 0;   // consecutive high/normal tasks taken by this worker

    if (queues_[0].pop(out))                                   // global high (strict)
    {
        ++since_low;
        return true;
    }
    // Valve: if we've taken many high/normal in a row, serve one low before normal/local.
    if (since_low >= low_valve_threshold && queues_[2].pop(out))
    {
        since_low = 0;
        return true;
    }
    if (local_normal_[static_cast<std::size_t>(worker_index)]->take(out))   // own deque, LIFO
    {
        ++since_low;
        return true;
    }
    if (queues_[1].pop(out))                                   // global normal (overflow + external)
    {
        ++since_low;
        return true;
    }

    // Steal a `normal` task from another worker's deque (start at a random victim, scan
    // around) - before global low, so a stealable normal outranks a global low task.
    int n = static_cast<int>(local_normal_.size());
    if (n > 1)
    {
        int start = static_cast<int>(next_rand() % static_cast<std::uint32_t>(n));
        for (int i = 0; i < n; ++i)
        {
            int v = start + i;
            if (v >= n)
                v -= n;
            if (v == worker_index)
                continue;
            if (local_normal_[static_cast<std::size_t>(v)]->steal(out))
            {
                ++since_low;
                return true;
            }
        }
    }

    if (queues_[2].pop(out))                                   // global low (nothing normal anywhere)
    {
        since_low = 0;
        return true;
    }
    return false;
}

int Scheduler::worker_count() const noexcept
{
    return static_cast<int>(workers_.size());
}

// Dispatch the idle wait for the configured policy. Returns true with a task in `out` to run;
// false means the worker parked-and-woke (or spin-yielded) and should re-scan / re-check quit.
bool Scheduler::wait_for_work(int worker_index, detail::Task_entry& out)
{
    switch (idle_policy_)
    {
    case Idle_policy::spin:
        std::this_thread::yield();   // never park; loop back and re-scan
        return false;
    case Idle_policy::spin_then_block:
        if (spin_scan(worker_index, out, spin_cycles_))
            return true;
        return park(worker_index, out);
    case Idle_policy::handoff:
        return handoff_wait(worker_index, out);
    }
    return false;
}

bool Scheduler::park(int worker_index, detail::Task_entry& out)
{
    // Snapshot the epoch, re-check for work/shutdown, then park only if the epoch is unchanged.
    // A submit that raced the failed scan bumped the epoch, so commit_wait returns at once. See
    // `Event_count`.
    std::uint32_t key = events_.prepare_wait();
    if (find_work(worker_index, out))
    {
        events_.cancel_wait();
        return true;
    }
    if (quit_.load(std::memory_order_acquire))
    {
        events_.cancel_wait();
        return false;
    }
    events_.commit_wait(key);
    return false;
}

bool Scheduler::spin_scan(int worker_index, detail::Task_entry& out, std::uint32_t cycles)
{
    for (std::uint32_t c = 0; c < cycles; ++c)
    {
        if (find_work(worker_index, out))
            return true;
        if (quit_.load(std::memory_order_acquire))
            return false;
        cpu_relax();
    }
    return false;
}

// Go-style spinner handoff. Become a spinner and scan for `spin_cycles_`; on finding work,
// relinquish the spinner role and - if we were the last spinner - wake a successor to keep the
// pool discovering work while we run. On an empty spin-out, drop the spinner role and park.
//
// `num_spinning_` is advisory: correctness rides the always-advanced epoch (`signal_submit`
// bumps it on every submit), so a park that races a submit is released by `commit_wait`
// regardless of what `num_spinning_` read. Thus relaxed ordering throughout, and an occasional
// stale count only costs a spurious wake or a bounded latency (the running worker re-scans when
// its task returns) - never a lost task.
bool Scheduler::handoff_wait(int worker_index, detail::Task_entry& out)
{
    num_spinning_.fetch_add(1, std::memory_order_relaxed);

    for (std::uint32_t c = 0; c < spin_cycles_; ++c)
    {
        if (find_work(worker_index, out))
        {
            if (num_spinning_.fetch_sub(1, std::memory_order_relaxed) == 1)
                events_.notify_one();   // we were the last spinner -> promote a successor
            return true;
        }
        if (quit_.load(std::memory_order_acquire))
        {
            num_spinning_.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }
        cpu_relax();
    }

    num_spinning_.fetch_sub(1, std::memory_order_relaxed);   // no longer spinning; park below
    return park(worker_index, out);
}

bool Scheduler::all_empty() const
{
    for (const auto& q : queues_)
    {
        if (!q.empty())
            return false;
    }
    for (const auto& d : local_normal_)
    {
        if (!d->empty())
            return false;
    }
    return true;
}

} // namespace ts
