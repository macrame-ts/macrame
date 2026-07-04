#pragma once

#include "event_count.h"
#include "mpmc_queue.h"
#include "priority.h"
#include "work_stealing_deque.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

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
    // Park immediately on the eventcount when no work is found. Producers wake one parked worker
    // on every submit. The simple baseline (== `spin_then_block` with zero spin cycles).
    block,
    // UE-style: an idle worker spins `spin_cycles` scans before parking, so a task arriving
    // during the spin window is grabbed with no wake latency. Producers still wake one parked
    // worker on every submit (spinning workers grab work without needing the wake).
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
    Idle_policy idle_policy = Idle_policy::block;
    // `spin_then_block`/`handoff`: number of `find_work` scans an idle worker spins before it
    // parks (UE's `WorkerSpinCycles` is ~53). Ignored by `spin`/`block`.
    uint32_t spin_cycles = 64;
};

using Task_func_ptr = void(*)(void* data);

namespace detail
{

// A queued task: the func + its data. Priority is no longer a field -- it is the queue the
// task lives in (one lock-free MPMC queue per priority, scanned high->low).
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
    // Defined in the .cpp -- `Worker_thread` is incomplete here.
    int worker_count() const noexcept;

private:
    // Find one task for worker `worker_index`, scanning: global high -> its own local deque
    // (LIFO, cache-hot) -> global normal -> global low -> steal `normal` from a random victim.
    // High stays strict (checked first). True if a task was found.
    bool find_work(int worker_index, detail::Task_entry& out);
    // Approximate: all global queues AND all local deques empty (racy; shutdown-drain check).
    bool all_empty() const;

    // Producer-side signal after a submit, per `idle_policy_` (see the enum).
    void signal_submit();

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

    // One lock-free MPMC queue per priority (index = the `Priority` enum value). `high`/`low`
    // are global-only; `normal` also has per-worker deques (below), with this as overflow +
    // the injector for external (non-worker) submits.
    std::array<detail::Mpmc_queue<detail::Task_entry>, detail::priority_count> queues_;
    // Per-worker Chase-Lev deque for `normal`: a worker's own `normal` submits go here (LIFO,
    // no shared cache line -- the producer fast path); thieves steal FIFO. One per worker.
    std::vector<std::unique_ptr<detail::Work_stealing_deque<detail::Task_entry>>> local_normal_;
    std::atomic<bool> quit_ = false;
    detail::Event_count events_;   // wakes idle (parking) workers; replaces the semaphore
    // `handoff` policy: advisory count of workers currently spinning. Correctness rides the
    // always-advanced epoch (see `signal_submit`), so this can be read/written relaxed -- it
    // only gates the wake syscall and the successor-promotion decision.
    std::atomic<int> num_spinning_ = 0;
    const Idle_policy idle_policy_;
    const std::uint32_t spin_cycles_;
    std::vector<detail::Worker_thread> workers_;
};
