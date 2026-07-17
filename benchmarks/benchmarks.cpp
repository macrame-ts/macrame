#include "benchmarks.h"
#include "ts/scheduler.h"
#include "ts/guarded.h"
#include "ts/static_task_graph.h"
#include "ts/access.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <tuple>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

// ~1 s per benchmark (warmup + reps measured rounds), median reported -- the
// numbers are stable enough to track for regression monitoring.
constexpr auto target = std::chrono::milliseconds(200);
constexpr int reps = 4;
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

// task bodies operate on a shared atomic passed as `void*` — no per-task allocation
void count_down(void* p) { static_cast<std::atomic<uint64_t>*>(p)->fetch_sub(1, std::memory_order_release); }
void count_up(void* p)   { static_cast<std::atomic<uint64_t>*>(p)->fetch_add(1, std::memory_order_release); }

// single producer floods a batch, then drains; measures raw submit+execute throughput
std::vector<double> bench_throughput(ts::Idle_policy policy)
{
    ts::Scheduler sched{ { .idle_policy = policy } };
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
// in `block` mode the pool re-sleeps between pings, so this captures wake latency.
std::vector<double> bench_wake_latency(ts::Idle_policy policy)
{
    ts::Scheduler sched{ { .idle_policy = policy } };
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

// N producer threads hammer submit concurrently; measures `queue_mutex_` contention
std::vector<double> bench_contention(ts::Idle_policy policy, unsigned producers)
{
    ts::Scheduler sched{ { .idle_policy = policy } };
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

// Fork-join / worker-submit throughput: tasks that spawn tasks. Unlike `contention` (which
// is EXTERNAL producers, all hitting the global queue), here the child submits come from
// WORKER bodies -- so with per-worker deques (M2 stage 3) they push to the worker's own local
// deque (no shared cache line) and idle workers steal. Raw scheduler API (no task-block alloc)
// to isolate scheduler throughput.
struct Fj_ctx
{
    ts::Scheduler* sched;
    std::atomic<int64_t> budget;   // remaining child tasks to spawn (across the whole tree)
    std::atomic<int64_t> done;
};
constexpr int fj_fanout = 4;

void fj_task(void* p)
{
    auto* c = static_cast<Fj_ctx*>(p);
    for (int k = 0; k < fj_fanout; ++k)
    {
        if (c->budget.fetch_sub(1, std::memory_order_relaxed) > 0)
            c->sched->submit(&fj_task, c);   // worker -> worker submit (this runs on a worker)
        else
            break;
    }
    c->done.fetch_add(1, std::memory_order_relaxed);
}

std::vector<double> bench_fork_join(ts::Idle_policy policy)
{
    ts::Scheduler sched{ { .idle_policy = policy } };
    return measure([&]() -> uint64_t
    {
        constexpr int64_t n = 100000;
        Fj_ctx c{ &sched, {}, {} };
        c.budget.store(n, std::memory_order_relaxed);
        c.done.store(0, std::memory_order_relaxed);
        sched.submit(&fj_task, &c);          // root (external submit)
        const int64_t total = n + 1;         // root + n spawned
        while (c.done.load(std::memory_order_acquire) < total)   // keeps `c` alive past all tasks
            std::this_thread::yield();
        return static_cast<uint64_t>(total);
    });
}

// --- public feature benchmarks --------------------------------------------

// Guarded write: serialized async writes through one object's pipe.
std::vector<double> bench_ts_write()
{
    ts::Guarded<uint64_t> obj{ 0 };
    constexpr uint64_t batch = 20000;
    return measure([&]() -> uint64_t
    {
        for (uint64_t i = 0; i < batch; ++i)
            obj.async([](uint64_t& v) { ++v; });
        obj.async([](const uint64_t& v) { return v; }).sync();   // FIFO drain
        return batch;
    });
}

// Guarded read: concurrent async reads through the reader/writer pipe.
std::vector<double> bench_ts_read()
{
    ts::Guarded<uint64_t> obj{ 7 };
    std::atomic<uint64_t> done{ 0 };
    constexpr uint64_t batch = 20000;
    return measure([&]() -> uint64_t
    {
        uint64_t base = done.load(std::memory_order_relaxed);
        for (uint64_t i = 0; i < batch; ++i)
            obj.async([&done](const uint64_t& v) { done.fetch_add(1, std::memory_order_relaxed); return v; });
        while (done.load(std::memory_order_acquire) < base + batch)
            std::this_thread::yield();
        return batch;
    });
}

// Task::then: continuation chain length K fired off one producer.
std::vector<double> bench_then()
{
    ts::Guarded<int> obj{ 0 };
    constexpr int chain = 50;
    return measure([&]() -> uint64_t
    {
        ts::Task<int> t = obj.async([](const int& v) { return v; });
        for (int k = 0; k < chain; ++k)
            t = t.then([](int v) { return v + 1; });
        t.sync();
        return chain;
    });
}

// when_all: typed join over 4 prerequisites + a consuming continuation.
std::vector<double> bench_when_all()
{
    ts::Guarded<int> a{ 1 }, b{ 2 }, c{ 3 }, d{ 4 };
    auto read = [](const int& v) { return v; };
    return measure([&]() -> uint64_t
    {
        ts::when_all(a.async(read), b.async(read), c.async(read), d.async(read))
            .then([](std::tuple<int, int, int, int>& r) { return std::get<0>(r); })
            .sync();
        return 1;
    });
}

// Static_task_graph: per-execute() dispatch + sync cost (8 independent nodes).
std::vector<double> bench_graph_execute()
{
    constexpr int nodes = 8;
    std::array<ts::Guarded<int>, nodes> stores{};
    ts::Static_task_graph g;
    for (auto& s : stores)
        g.add_node([](int& v) { ++v; }, s);
    g.compile();
    return measure([&]() -> uint64_t
    {
        g.execute().sync();
        return 1;
    });
}

// Access harness: cost of a guarded method call (TS_CHECK_ACCESS).
struct Guarded
{
    int v = 0;
    void touch() { TS_CHECK_ACCESS(); ++v; }
};

std::vector<double> bench_harness()
{
    Guarded g;
    ts::Access_context ctx;
    ctx.add(&g, ts::Access::read_write);
    constexpr uint64_t calls = 200000;
    return measure([&]() -> uint64_t
    {
        ts::Access_scope scope(ctx);
        for (uint64_t i = 0; i < calls; ++i)
            g.touch();
        return calls;
    });
}

} // namespace

void run_benchmarks()
{
    unsigned hw = std::thread::hardware_concurrency();
    std::printf("\nbenchmarks: %d reps x %lldms target, %u hw threads\n",
        reps, static_cast<long long>(target.count()), hw);

    std::printf("\nthroughput (1 producer, empty tasks):\n");
    report("spin",    bench_throughput(ts::Idle_policy::spin));
    report("s+block", bench_throughput(ts::Idle_policy::spin_then_block));
    report("handoff", bench_throughput(ts::Idle_policy::handoff));

    std::printf("\nwake latency (1 task in flight):\n");
    report("spin",    bench_wake_latency(ts::Idle_policy::spin));
    report("s+block", bench_wake_latency(ts::Idle_policy::spin_then_block));
    report("handoff", bench_wake_latency(ts::Idle_policy::handoff));

    std::printf("\ncontention (%u producers):\n", hw);
    report("spin",    bench_contention(ts::Idle_policy::spin, hw));
    report("s+block", bench_contention(ts::Idle_policy::spin_then_block, hw));
    report("handoff", bench_contention(ts::Idle_policy::handoff, hw));

    std::printf("\nfork-join (worker->worker submits, fanout %d):\n", fj_fanout);
    report("spin",    bench_fork_join(ts::Idle_policy::spin));
    report("s+block", bench_fork_join(ts::Idle_policy::spin_then_block));
    report("handoff", bench_fork_join(ts::Idle_policy::handoff));

    std::printf("\nfeatures:\n");
    report("ts_write", bench_ts_write());
    report("ts_read", bench_ts_read());
    report("then", bench_then());
    report("when_all", bench_when_all());
    report("graph", bench_graph_execute());
    report("harness", bench_harness());
}
