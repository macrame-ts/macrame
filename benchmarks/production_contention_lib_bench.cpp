#include "benchmarks.h"
#include "bench_harness.h"

#include "ts/deferred.h"
#include "ts/guarded.h"
#include "ts/parallel_for.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{

// --- the same production-contention workload, architected on the library --------------
//
// The companion to `production_contention_bench.cpp` (keep the workload constants in
// sync): same wave-released items, same private work and jitter, same touch work, same
// rare slow maintenance op, same totals. What changes is the architecture - this is the
// workload built from scratch the way this library intends, i.e. the research's fix
// hierarchy (docs/internals/lock-contention-research.md §4) applied as primitives rather than
// bespoke heroics:
//
//  - every touch is a grant-free stage into a `Parallel_recorder` (per-worker lanes) -
//    no lock is ever taken on the item path, so there is nothing to convoy on;
//  - the wave is the frame: one `parallel_for` per wave (its join is the wave barrier),
//    one fire-and-forget `commit()` at wave end - the singleton's serial lane is paid
//    once per wave, and the apply overlaps the next wave's private work on the pool;
//  - the slow maintenance op runs inside the apply - scheduled work no worker queues
//    behind; its cost converts from stalls into STALENESS, reported here as the apply
//    lag (wave start -> batch applied), the model's honest price;
//  - workers = cores only. There is no oversubscribed row because there is nothing for
//    oversubscription to fix - no worker ever parks on data access. That asymmetry with
//    the baseline is a result, not an omission.
//
// Reported per row: throughput (items / time-to-all-applied - work does not count until
// the singleton saw it), worker-side item latency p50/p99/p999 (born -> private work +
// stage complete; the tail-immunity claim), and apply lag mean/max (the staleness). The
// per-command closure allocation (~30 ns staged, SBO) is today's cost, the typed-lane
// tier's before-story.

constexpr int wave_size = 1024;
constexpr int waves_per_batch = 8;
constexpr int items_per_batch = wave_size * waves_per_batch;
constexpr int touch_ns = 200;
constexpr int slow_op_ns = 100000;
constexpr int private_ns = 8000;

void spin_ns(int ns)
{
    if (ns <= 0)
        return;
    auto end = Clock::now() + std::chrono::nanoseconds(ns);
    while (Clock::now() < end)
    {
    }
}

int jittered(int base, int id)
{
    std::uint64_t r = (static_cast<std::uint64_t>(id) + 1) * 0x9e3779b97f4a7c15ull;
    int span = base / 2;
    return base - base / 4 + static_cast<int>((r >> 40) % static_cast<unsigned>(span < 1 ? 1 : span));
}

struct Singleton
{
    std::uint64_t state = 0;
    double lag_sum_us = 0;
    double lag_max_us = 0;
    int lag_count = 0;
};

struct Row_result
{
    std::vector<double> ops;
    double p50 = 0, p99 = 0, p999 = 0;   // us, worker-side, final batch
    double lag_mean = 0, lag_max = 0;    // us, apply lag, final batch
};

Row_result bench_row(int slow_every)
{
    ts::Guarded<Singleton> service{ ts::Named{} };
    ts::Deferred<Singleton> staged{ service };
    ts::Parallel_recorder<Singleton> rec = staged.parallel_recorder();
    std::vector<std::uint32_t> latency(items_per_batch);

    auto run_batch = [&]() -> double
    {
        service.access([](Singleton& s)
        {
            s.lag_sum_us = 0;
            s.lag_max_us = 0;
            s.lag_count = 0;
        }).sync();

        auto t0 = Clock::now();
        ts::Task<void> last_commit;
        for (int w = 0; w < waves_per_batch; ++w)
        {
            auto wave_start = Clock::now();
            int base = w * wave_size;
            ts::parallel_for(wave_size, [&, base](int k)
            {
                int id = base + k;
                auto born = Clock::now();
                spin_ns(jittered(private_ns / 2, id));
                bool slow = slow_every > 0 && id % slow_every == slow_every - 1;
                rec.stage([slow](Singleton& s)
                {
                    spin_ns(slow ? slow_op_ns : touch_ns);   // the touch work, moved to the apply lane
                    ++s.state;
                });
                spin_ns(jittered(private_ns / 2, id + 7919));
                latency[static_cast<std::size_t>(id)] = static_cast<std::uint32_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - born).count());
            });
            // The wave's staleness marker: applied last (intra-recorder FIFO), stamps
            // how long after the wave's start its whole batch became visible.
            rec.stage([wave_start](Singleton& s)
            {
                double us = std::chrono::duration<double, std::micro>(Clock::now() - wave_start).count();
                s.lag_sum_us += us;
                if (us > s.lag_max_us)
                    s.lag_max_us = us;
                ++s.lag_count;
            });
            last_commit = staged.commit();   // fire-and-forget; the apply overlaps the next wave
        }
        last_commit.sync();                  // pipe FIFO: all prior applies settled too
        return std::chrono::duration<double>(Clock::now() - t0).count();
    };

    Row_result r;
    run_batch();   // warmup
    for (int rep = 0; rep < reps; ++rep)
        r.ops.push_back(items_per_batch / run_batch());

    service.access([&r](const Singleton& s)
    {
        r.lag_mean = s.lag_count > 0 ? s.lag_sum_us / s.lag_count : 0.0;
        r.lag_max = s.lag_max_us;
    }).sync();

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
    return r;
}

void report_row(const char* name, const Row_result& r)
{
    report(name, r.ops);
    std::printf("  %-10s p50/p99/p999: %5.1f / %6.1f / %7.1f us   apply lag mean/max: %6.1f / %6.1f us\n",
        name, r.p50, r.p99, r.p999, r.lag_mean, r.lag_max);
}

} // namespace

void run_production_contention_lib_bench()
{
    unsigned hw = std::thread::hardware_concurrency();
    std::printf("\nproduction contention, library architecture (same workload staged: grant-free"
                " touches,\n  one commit per wave, slow op runs in the apply lane; workers = cores"
                " only - nothing parks,\n  so there is nothing for oversubscription to fix; %u"
                " workers):\n", hw);
    report_row("lib none", bench_row(0));
    report_row("lib /1024", bench_row(1024));
    report_row("lib /256", bench_row(256));
}
