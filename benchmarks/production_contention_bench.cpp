#include "benchmarks.h"
#include "bench_harness.h"

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

// --- the generalized production lock-contention case ----------------------------------
//
// Distilled from 41 cited production reports (docs/internals/lock-contention-research.md): parallel
// work items, arriving in bursts, each performing private work plus one tiny exclusive
// touch of a shared singleton service - which occasionally executes a much longer
// maintenance operation under the same lock. Pure mocks on purpose: a plain std::thread
// pool and a std::mutex, no library machinery anywhere - this group is the baseline
// exhibit of the phenomenon itself, the fixed reference any library-side comparison must
// measure against.
//
// The elements, each load-bearing per the research:
//  - one hot singleton lock (the registry/pool/queue/metrics/allocator shape - the hot
//    lock in production is almost always a process-wide singleton on every item's path);
//  - a ~200 ns touch amid ~8 us of private work: offered load on the lock stays below
//    saturation - steady arrivals mostly miss each other, which is why the healthy
//    baseline ships every day;
//  - a rare slow operation (~100 us) under the same lock - the rehash / flush / scan that
//    grew with state; the trigger of the convoy class, where p99/p999 detaches while the
//    mean looks fine;
//  - wave arrivals (a burst of items released at once, the frame / request-wave shape):
//    bursts synchronize arrivals so the queue on the lock does not drain between them;
//  - threads = cores and threads = 2 x cores: oversubscription turns waiting into
//    convoying (a preempted holder needs a runnable competitor).
//
// Reported per row: throughput, item service latency p50/p99/p999 (claim to completion,
// lock waits included), and the lock-wait share of total thread-time - the "% of CPU in
// locks" number production postmortems are written in. Per-item jitter de-aligns touches
// within a wave, so contention comes from the workload's shape, not from lockstep.

constexpr int wave_size = 1024;
constexpr int waves_per_batch = 8;
constexpr int items_per_batch = wave_size * waves_per_batch;
constexpr int touch_ns = 200;
constexpr int slow_op_ns = 100000;
constexpr int private_ns = 8000;   // split around the touch, +-25% jitter

void spin_ns(int ns)
{
    if (ns <= 0)
        return;
    auto end = Clock::now() + std::chrono::nanoseconds(ns);
    while (Clock::now() < end)
    {
    }
}

// Deterministic per-item jitter in [-25%, +25%] of `base`.
int jittered(int base, int id)
{
    std::uint64_t r = (static_cast<std::uint64_t>(id) + 1) * 0x9e3779b97f4a7c15ull;
    int span = base / 2;
    return base - base / 4 + static_cast<int>((r >> 40) % static_cast<unsigned>(span < 1 ? 1 : span));
}

struct alignas(64) Singleton
{
    std::mutex m;
    std::uint64_t state = 0;
};

struct alignas(64) Wait_slot
{
    std::uint64_t ns = 0;
};

struct Row_result
{
    std::vector<double> ops;        // per measured batch, items/sec
    double p50 = 0, p99 = 0, p999 = 0;   // us, final batch
    double wait_share = 0;          // lock-wait / total thread-time, final batch
};

Row_result bench_row(int threads, int slow_every)
{
    Singleton service;
    std::vector<Wait_slot> wait(static_cast<std::size_t>(threads));
    std::vector<std::uint32_t> latency(items_per_batch);

    std::atomic<int> next{ wave_size };     // claim index within the current wave (starts drained)
    std::atomic<int> wave_base{ 0 };        // first item id of the current wave
    std::atomic<std::uint64_t> epoch{ 0 };  // bumped to release a wave
    std::atomic<int> done{ 0 };             // items completed in the current wave
    std::atomic<bool> stop{ false };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t)
    {
        pool.emplace_back([&, t]
        {
            std::uint64_t seen = 0;
            for (;;)
            {
                epoch.wait(seen, std::memory_order_acquire);
                std::uint64_t e = epoch.load(std::memory_order_acquire);
                if (e == seen)
                    continue;
                seen = e;
                if (stop.load(std::memory_order_acquire))
                    break;
                int base = wave_base.load(std::memory_order_acquire);
                for (;;)
                {
                    int k = next.fetch_add(1, std::memory_order_relaxed);
                    if (k >= wave_size)
                        break;
                    int id = base + k;
                    auto born = Clock::now();
                    spin_ns(jittered(private_ns / 2, id));
                    auto want = Clock::now();
                    {
                        std::scoped_lock lock(service.m);
                        wait[static_cast<std::size_t>(t)].ns += static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - want).count());
                        spin_ns(slow_every > 0 && id % slow_every == slow_every - 1 ? slow_op_ns : touch_ns);
                        ++service.state;
                    }
                    spin_ns(jittered(private_ns / 2, id + 7919));
                    latency[static_cast<std::size_t>(id)] = static_cast<std::uint32_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - born).count());
                    done.fetch_add(1, std::memory_order_release);
                }
            }
        });
    }

    auto run_batch = [&]() -> double
    {
        for (Wait_slot& w : wait)
        {
            w.ns = 0;
        }
        auto t0 = Clock::now();
        for (int w = 0; w < waves_per_batch; ++w)
        {
            wave_base.store(w * wave_size, std::memory_order_relaxed);
            done.store(0, std::memory_order_relaxed);
            next.store(0, std::memory_order_release);
            epoch.fetch_add(1, std::memory_order_release);
            epoch.notify_all();
            while (done.load(std::memory_order_acquire) != wave_size)
            {
                std::this_thread::yield();
            }
        }
        return std::chrono::duration<double>(Clock::now() - t0).count();
    };

    Row_result r;
    run_batch();   // warmup
    double last_sec = 0;
    for (int rep = 0; rep < reps; ++rep)
    {
        last_sec = run_batch();
        r.ops.push_back(items_per_batch / last_sec);
    }

    std::uint64_t wait_sum = 0;
    for (const Wait_slot& w : wait)
    {
        wait_sum += w.ns;
    }
    r.wait_share = static_cast<double>(wait_sum) / (static_cast<double>(threads) * last_sec * 1e9);

    std::vector<std::uint32_t> lat = latency;
    auto pct = [&](double p) -> double
    {
        std::size_t k = static_cast<std::size_t>(p * (lat.size() - 1));
        std::nth_element(lat.begin(), lat.begin() + static_cast<std::ptrdiff_t>(k), lat.end());
        return lat[k] / 1000.0;
    };
    r.p50 = pct(0.50);
    r.p99 = pct(0.99);
    r.p999 = pct(0.999);

    stop.store(true, std::memory_order_release);
    epoch.fetch_add(1, std::memory_order_release);
    epoch.notify_all();
    for (std::thread& t : pool)
    {
        t.join();
    }
    return r;
}

void report_row(const char* name, const Row_result& r)
{
    report(name, r.ops);
    std::printf("  %-10s p50/p99/p999: %5.1f / %6.1f / %7.1f us   lock-wait share: %4.1f%%\n",
        name, r.p50, r.p99, r.p999, r.wait_share * 100.0);
}

} // namespace

void run_production_contention_bench()
{
    unsigned hw = std::thread::hardware_concurrency();
    double item_ns = private_ns + touch_ns;
    std::printf("\nproduction contention (one singleton, %dns touches amid %dns private work,"
                " %d-item waves,\n  rare %dus slow op under the same lock; plain std::thread +"
                " std::mutex, no library code;\n  core bound ~%.2f M items/s at %u threads):\n",
        touch_ns, private_ns, wave_size, slow_op_ns / 1000, hw * 1e3 / item_ns, hw);

    struct { const char* name; int threads; int slow; } rows[] = {
        { "1x none",  static_cast<int>(hw),     0 },
        { "1x /1024", static_cast<int>(hw),     1024 },
        { "1x /256",  static_cast<int>(hw),     256 },
        { "2x none",  static_cast<int>(2 * hw), 0 },
        { "2x /1024", static_cast<int>(2 * hw), 1024 },
        { "2x /256",  static_cast<int>(2 * hw), 256 },
    };
    for (const auto& row : rows)
    {
        report_row(row.name, bench_row(row.threads, row.slow));
    }
}
