#include "scheduler.h"
#include "worker_thread.h"

#include <cstddef>
#include <cstdint>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    #include <immintrin.h>   // _mm_pause
#endif

namespace ts
{

thread_local Scheduler* current_scheduler = nullptr;
thread_local int current_worker_index = -1;

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
{
    uint32_t num_threads = config.num_threads;
    if (num_threads == 0)
        num_threads = std::thread::hardware_concurrency();

    // Per-worker deques must exist before any worker starts (workers steal from all of them).
    local_normal_.reserve(num_threads);
    for (uint32_t i = 0; i < num_threads; ++i)
        local_normal_.push_back(std::make_unique<detail::Work_stealing_deque<detail::Task_entry>>());

    workers_.reserve(num_threads);
    for (uint32_t i = 0; i < num_threads; ++i)
        workers_.emplace_back(*this, static_cast<int>(i));
}

Scheduler::~Scheduler()
{
    quit_.store(true, std::memory_order_release);

    // wake every parked worker so it observes `quit_` and drains/exits (a not-yet-parked
    // worker sees the bumped epoch in its re-check / commit_wait, so none is missed). Every
    // parking policy needs this; only `spin` never parks.
    if (idle_policy_ != Idle_policy::spin)
        events_.notify_all();
}

void Scheduler::submit(Task_func_ptr func, void* data, Priority priority)
{
    // A worker submitting `normal` to its OWN scheduler pushes to its local deque (LIFO,
    // cache-hot, no shared cache line -- the producer fast path). External/non-normal submits,
    // and a full local deque, go to the global queue for the priority.
    if (priority == Priority::normal && current_scheduler == this && current_worker_index >= 0
        && local_normal_[static_cast<std::size_t>(current_worker_index)]->push({ func, data }))
    {
        signal_submit();   // wake a thief to help (no-op if none parked)
        return;
    }

    queues_[static_cast<std::size_t>(priority)].push({ func, data });

    signal_submit();
}

// Producer-side wake, per policy. `block`/`spin_then_block` wake one parked worker every submit;
// `handoff` always advances the epoch (cheap -- releases any worker about to park) but issues the
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
// normal -> global low -> steal `normal` from a random victim. A per-worker aging counter
// forces `low` ahead of normal once in a while (the starvation valve).
bool Scheduler::find_work(int worker_index, detail::Task_entry& out)
{
    static thread_local int since_low = 0;   // consecutive high/normal tasks taken by this worker

    if (queues_[0].pop(out))                                   // global high (strict)
    {
        ++since_low;
        return true;
    }
    // Valve: if we've taken many high/normal in a row, serve one low BEFORE normal/local.
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
    if (queues_[2].pop(out))                                   // global low (normal was empty anyway)
    {
        since_low = 0;
        return true;
    }

    // Steal a `normal` task from another worker's deque (start at a random victim, scan around).
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
    // Snapshot the epoch, RE-CHECK for work/shutdown, then park only if the epoch is unchanged.
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
// relinquish the spinner role and -- if we were the last spinner -- wake a successor to keep the
// pool discovering work while we run. On an empty spin-out, drop the spinner role and park.
//
// `num_spinning_` is advisory: correctness rides the always-advanced epoch (`signal_submit`
// bumps it on every submit), so a park that races a submit is released by `commit_wait`
// regardless of what `num_spinning_` read. Thus relaxed ordering throughout, and an occasional
// stale count only costs a spurious wake or a bounded latency (the running worker re-scans when
// its task returns) -- never a lost task.
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
        if (!q.empty())
            return false;
    for (const auto& d : local_normal_)
        if (!d->empty())
            return false;
    return true;
}

} // namespace ts
