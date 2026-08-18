#include "benchmarks.h"
#include "bench_harness.h"

#include "ts/guarded.h"
#include "ts/parallel_for.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{

// --- access control: std::mutex pool vs Guarded::access ------------------------------
//
// One workload, two access-control mechanisms. `ac_stores` stores; a pregenerated job
// stream (deterministic LCG, fixed seed): each job takes store `target` and spins W us
// under it; a hot share h% of jobs target store 0, the rest spread uniformly over the
// others. Both worlds claim jobs from one shared atomic counter - a static per-thread
// job split would impose head-of-line blocking the mutex side's real users don't
// suffer, so the shared counter is what keeps the comparison honest. Completion is one
// shared `done` counter incremented in the job body - the same cache-line cost in both
// worlds.
//
// The asymmetry under test, stated plainly: at the moment of contention the mutex
// claimer waits (its core idles unless the pool is oversubscribed), while the `access`
// claimer leaves the job queued on the store's pipe and claims the next - the job
// waits, not the worker. That is the architectural difference being measured, not
// bench bias. The prices on the `access` side: up to h*J blocks queued on the hot pipe
// at once, and today's `access` allocates its block even on the inline path - this
// group is the before baseline for the caller-owned `Access_op` redesign. Uncontended,
// the mutex is expected to win by the raw mechanism delta (an uncontended lock is tens
// of ns; an `access` is a block allocation + pipe claim + context install), and the
// W=0 rows show exactly that with no work to hide it. What the mutex rows do not buy:
// per-store FIFO order and harness coverage, which the `access` rows carry at the
// prices above. A `try_lock`-and-requeue pool - the strongest lock-side design, and
// structurally a hand-rolled pipe - is noted as future work, not built. The
// scheduler's workers are parked during the mutex rows (`spin_then_block` parks after
// a brief spin) - negligible interference.
//
// Physical ceiling, printed with the group: jobs on one store serialize, so no
// mechanism can beat max(hot chain, core bound) - jobs/s = hw/W at h=0, else
// min(hw/W, 1/(h*W)). A measured row above its ceiling means the bench is broken.

constexpr int ac_stores = 64;

struct alignas(64) Locked_store
{
    std::mutex m;
    std::uint64_t v = 0;
};

struct Access_payload
{
    std::uint64_t v = 0;
};

// The same busy-work in both worlds; W = 0 degenerates to nothing (the mechanism floor).
void spin_us(int us)
{
    if (us <= 0)
        return;
    auto end = Clock::now() + std::chrono::microseconds(us);
    while (Clock::now() < end)
    {
    }
}

// h% of jobs hit store 0; the rest spread over 1..ac_stores-1.
std::vector<std::uint16_t> make_targets(int count, int hot_percent)
{
    std::vector<std::uint16_t> targets(static_cast<std::size_t>(count));
    std::uint64_t r = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < count; ++i)
    {
        r = r * 6364136223846793005ull + 1442695040888963407ull;
        bool hot = static_cast<int>((r >> 33) % 100) < hot_percent;
        targets[static_cast<std::size_t>(i)] =
            hot ? 0 : static_cast<std::uint16_t>(1 + ((r >> 13) % (ac_stores - 1)));
    }
    return targets;
}

struct alignas(64) Ac_counter
{
    std::atomic<int> v{ 0 };
};

// Persistent epoch-gated pool: the threads are created once per series and parked on
// an atomic wait between reps, so thread creation never pollutes a measured window.
// `running` is the rep barrier - the driver resets the counters only after every
// claimer of the previous rep has parked (a straggler between its last claim check
// and the park must not see the reset mid-flight).
std::vector<double> bench_mutex_row(int threads, int hot_percent, int work_us, int jobs_per_batch)
{
    auto stores = std::make_unique<Locked_store[]>(ac_stores);
    std::vector<std::uint16_t> targets = make_targets(jobs_per_batch, hot_percent);
    Ac_counter next, done;
    std::atomic<std::uint64_t> epoch{ 0 };
    std::atomic<int> running{ 0 };
    std::atomic<bool> stop{ false };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t)
    {
        pool.emplace_back([&]
        {
            std::uint64_t seen = 0;
            for (;;)
            {
                epoch.wait(seen, std::memory_order_acquire);
                std::uint64_t e = epoch.load(std::memory_order_acquire);
                if (e == seen)
                    continue;   // spurious wake
                seen = e;
                if (stop.load(std::memory_order_acquire))
                    break;
                running.fetch_add(1, std::memory_order_acq_rel);
                for (;;)
                {
                    int j = next.v.fetch_add(1, std::memory_order_relaxed);
                    if (j >= jobs_per_batch)
                        break;
                    Locked_store& s = stores[targets[static_cast<std::size_t>(j)]];
                    {
                        std::scoped_lock lock(s.m);
                        spin_us(work_us);
                        ++s.v;
                    }
                    done.v.fetch_add(1, std::memory_order_release);
                }
                running.fetch_sub(1, std::memory_order_release);
            }
        });
    }

    auto ops = measure([&]() -> uint64_t
    {
        next.v.store(0, std::memory_order_relaxed);
        done.v.store(0, std::memory_order_relaxed);
        epoch.fetch_add(1, std::memory_order_release);
        epoch.notify_all();
        while (done.v.load(std::memory_order_acquire) != jobs_per_batch)
        {
            std::this_thread::yield();
        }
        while (running.load(std::memory_order_acquire) != 0)
        {
            std::this_thread::yield();
        }
        return static_cast<uint64_t>(jobs_per_batch);
    });

    stop.store(true, std::memory_order_release);
    epoch.fetch_add(1, std::memory_order_release);
    epoch.notify_all();
    for (std::thread& t : pool)
    {
        t.join();
    }
    return ops;
}

// `access` fire-and-forget: inline when the store is free, queued on its pipe when
// not - the claimer never waits. Claimers are `parallel_for` items with a bounded
// per-item claim budget, so workers rotate through the scheduler between slices and
// drain queued (contended) accesses interleaved with claiming - the batch-shaped
// equivalent of steady-state submission. One monolithic claim loop per worker would
// hold every worker until all claims were placed and push the whole hot backlog to
// the end of the batch, penalising `access` for the bench's own shape.
std::vector<double> bench_access_row(int hot_percent, int work_us, int jobs_per_batch, unsigned hw)
{
    std::vector<std::unique_ptr<ts::Guarded<Access_payload>>> stores;
    stores.reserve(ac_stores);
    for (int i = 0; i < ac_stores; ++i)
    {
        stores.push_back(std::make_unique<ts::Guarded<Access_payload>>(ts::Named{}));
    }
    std::vector<std::uint16_t> targets = make_targets(jobs_per_batch, hot_percent);
    Ac_counter next, done;

    int slices = static_cast<int>(hw) * 8;
    int budget = jobs_per_batch / slices + 1;

    return measure([&]() -> uint64_t
    {
        next.v.store(0, std::memory_order_relaxed);
        done.v.store(0, std::memory_order_relaxed);
        ts::parallel_for(slices, [&](int)
        {
            for (int k = 0; k < budget; ++k)
            {
                int j = next.v.fetch_add(1, std::memory_order_relaxed);
                if (j >= jobs_per_batch)
                    break;
                (void)stores[targets[static_cast<std::size_t>(j)]]->access([&done, work_us](Access_payload& p)
                {
                    spin_us(work_us);
                    ++p.v;
                    done.v.fetch_add(1, std::memory_order_release);
                });
            }
        });
        while (done.v.load(std::memory_order_acquire) != jobs_per_batch)
        {
            std::this_thread::yield();   // queued contended accesses may still be draining
        }
        return static_cast<uint64_t>(jobs_per_batch);
    });
}

} // namespace

void run_access_control_bench()
{
    unsigned hw = std::thread::hardware_concurrency();
    constexpr int ac_J = 20000;        // ~5 ms of work per batch at W=2us on 8 cores
    constexpr int ac_J0 = 100000;      // W=0: mechanism-only, bigger batch
    double w_s = 2e-6;
    double ceil0 = hw / w_s / 1e6;
    double ceil25 = std::min(ceil0, 1.0 / (0.25 * w_s) / 1e6);
    double ceil50 = std::min(ceil0, 1.0 / (0.50 * w_s) / 1e6);
    std::printf("\naccess control: std::mutex pool vs Guarded::access (%d stores, W=2us, hot share h;"
                " ceilings %.2f / %.2f / %.2f M/s for h=0/25/50 - no row may exceed its ceiling):\n",
        ac_stores, ceil0, ceil25, ceil50);
    report("mtx1x h0", bench_mutex_row(static_cast<int>(hw), 0, 2, ac_J));
    report("mtx1x h25", bench_mutex_row(static_cast<int>(hw), 25, 2, ac_J));
    report("mtx1x h50", bench_mutex_row(static_cast<int>(hw), 50, 2, ac_J));
    report("mtx2x h0", bench_mutex_row(2 * static_cast<int>(hw), 0, 2, ac_J));
    report("mtx2x h25", bench_mutex_row(2 * static_cast<int>(hw), 25, 2, ac_J));
    report("mtx2x h50", bench_mutex_row(2 * static_cast<int>(hw), 50, 2, ac_J));
    report("mtx4x h0", bench_mutex_row(4 * static_cast<int>(hw), 0, 2, ac_J));
    report("mtx4x h25", bench_mutex_row(4 * static_cast<int>(hw), 25, 2, ac_J));
    report("mtx4x h50", bench_mutex_row(4 * static_cast<int>(hw), 50, 2, ac_J));
    report("acc h0", bench_access_row(0, 2, ac_J, hw));
    report("acc h25", bench_access_row(25, 2, ac_J, hw));
    report("acc h50", bench_access_row(50, 2, ac_J, hw));
    std::printf("  mechanism floor (W=0, h=0 - no work to hide the per-job cost):\n");
    report("mtx1x W0", bench_mutex_row(static_cast<int>(hw), 0, 0, ac_J0));
    report("acc W0", bench_access_row(0, 0, ac_J0, hw));
}
