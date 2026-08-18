#include "benchmarks.h"
#include "bench_harness.h"

#include "ts/guarded.h"   // global_scheduler
#include "ts/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{

// task bodies operate on a shared atomic passed as `void*` - no per-task allocation
void count_down(void* p) { static_cast<std::atomic<uint64_t>*>(p)->fetch_sub(1, std::memory_order_release); }
void count_up(void* p)   { static_cast<std::atomic<uint64_t>*>(p)->fetch_add(1, std::memory_order_release); }

// single producer floods a batch, then drains; measures raw submit+execute throughput
std::vector<double> bench_throughput(ts::Idle_policy policy)
{
    ts::Scheduler_scope scope{ { .idle_policy = policy } };
    ts::Scheduler& sched = ts::global_scheduler();
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
    ts::Scheduler_scope scope{ { .idle_policy = policy } };
    ts::Scheduler& sched = ts::global_scheduler();
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
    ts::Scheduler_scope scope{ { .idle_policy = policy } };
    ts::Scheduler& sched = ts::global_scheduler();
    std::atomic<uint64_t> completed{ 0 };

    auto run_once = [&]() -> uint64_t
    {
        std::atomic<bool> stop{ false };
        std::atomic<uint64_t> submitted{ 0 };

        {
            std::vector<std::jthread> threads;
            for (unsigned i = 0; i < producers; ++i)
            {
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
            }

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
// is external producers, all hitting the global queue), here the child submits come from
// worker bodies - so with per-worker deques (M2 stage 3) they push to the worker's own local
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
    ts::Scheduler_scope scope{ { .idle_policy = policy } };
    ts::Scheduler& sched = ts::global_scheduler();
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

} // namespace

void run_scheduler_bench()
{
    unsigned hw = std::thread::hardware_concurrency();

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
}
