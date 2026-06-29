#include "benchmarks.h"
#include "scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

constexpr auto target = std::chrono::milliseconds(500);
constexpr int reps = 5;
constexpr int warmup = 1;

// run `work` (returns ops completed per call) repeatedly until `target` elapses,
// warmup + reps times; returns ops/sec per measured rep
template<typename Work>
std::vector<double> measure(Work&& work)
{
    std::vector<double> ops_per_sec;
    for (int r = 0; r < warmup + reps; ++r)
    {
        uint64_t ops = 0;
        auto t0 = Clock::now();
        do { ops += work(); } while (Clock::now() - t0 < target);
        double sec = std::chrono::duration<double>(Clock::now() - t0).count();
        if (r >= warmup)
            ops_per_sec.push_back(ops / sec);
    }
    return ops_per_sec;
}

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

void report(const char* name, std::vector<double> ops)
{
    double mn = *std::min_element(ops.begin(), ops.end());
    double mx = *std::max_element(ops.begin(), ops.end());
    double md = median(ops);
    std::printf("  %-10s %9.2f M/s   [min %7.2f, max %7.2f]   %9.1f ns/op\n",
        name, md / 1e6, mn / 1e6, mx / 1e6, 1e9 / md);
}

// task bodies operate on a shared atomic passed as void* — no per-task allocation
void count_down(void* p) { static_cast<std::atomic<uint64_t>*>(p)->fetch_sub(1, std::memory_order_release); }
void count_up(void* p)   { static_cast<std::atomic<uint64_t>*>(p)->fetch_add(1, std::memory_order_release); }

// single producer floods a batch, then drains; measures raw submit+execute throughput
std::vector<double> bench_throughput(Idle_policy policy)
{
    Scheduler sched{ { .idle_policy = policy } };
    std::atomic<uint64_t> remaining{ 0 };
    constexpr uint64_t batch = 50000;

    return measure([&]() -> uint64_t
    {
        remaining.store(batch, std::memory_order_relaxed);
        for (uint64_t i = 0; i < batch; ++i)
            sched.submit(count_down, &remaining);
        while (remaining.load(std::memory_order_acquire))
            std::this_thread::yield();
        return batch;
    });
}

// one task in flight at a time; ns/op is the submit->execute round trip.
// in block mode the pool re-sleeps between pings, so this captures wake latency.
std::vector<double> bench_wake_latency(Idle_policy policy)
{
    Scheduler sched{ { .idle_policy = policy } };
    std::atomic<uint64_t> done{ 0 };

    return measure([&]() -> uint64_t
    {
        uint64_t cur = done.load(std::memory_order_relaxed);
        sched.submit(count_up, &done);
        while (done.load(std::memory_order_acquire) == cur)
            std::this_thread::yield();
        return 1;
    });
}

// N producer threads hammer submit concurrently; measures queue_mutex_ contention
std::vector<double> bench_contention(Idle_policy policy, unsigned producers)
{
    Scheduler sched{ { .idle_policy = policy } };
    std::atomic<uint64_t> completed{ 0 };

    auto run_once = [&]() -> uint64_t
    {
        std::atomic<bool> stop{ false };
        std::atomic<uint64_t> submitted{ 0 };

        {
            std::vector<std::jthread> threads;
            for (unsigned i = 0; i < producers; ++i)
                threads.emplace_back([&]
                {
                    uint64_t local = 0;
                    while (!stop.load(std::memory_order_relaxed))
                    {
                        sched.submit(count_up, &completed);
                        ++local;
                    }
                    submitted.fetch_add(local, std::memory_order_relaxed);
                });

            std::this_thread::sleep_for(target);
            stop.store(true, std::memory_order_relaxed);
        } // join producers

        uint64_t total = submitted.load(std::memory_order_relaxed);
        while (completed.load(std::memory_order_acquire) < total)
            std::this_thread::yield();
        completed.store(0, std::memory_order_relaxed);
        return total;
    };

    std::vector<double> ops_per_sec;
    for (int r = 0; r < warmup + reps; ++r)
    {
        auto t0 = Clock::now();
        uint64_t ops = run_once();
        double sec = std::chrono::duration<double>(Clock::now() - t0).count();
        if (r >= warmup)
            ops_per_sec.push_back(ops / sec);
    }
    return ops_per_sec;
}

} // namespace

void run_benchmarks()
{
    unsigned hw = std::thread::hardware_concurrency();
    std::printf("\nbenchmarks: %d reps x %lldms target, %u hw threads\n",
        reps, static_cast<long long>(target.count()), hw);

    std::printf("\nthroughput (1 producer, empty tasks):\n");
    report("block", bench_throughput(Idle_policy::block));
    report("spin",  bench_throughput(Idle_policy::spin));

    std::printf("\nwake latency (1 task in flight):\n");
    report("block", bench_wake_latency(Idle_policy::block));
    report("spin",  bench_wake_latency(Idle_policy::spin));

    std::printf("\ncontention (%u producers):\n", hw);
    report("block", bench_contention(Idle_policy::block, hw));
    report("spin",  bench_contention(Idle_policy::spin, hw));
}
