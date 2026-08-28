#include "scheduler_scope.h"
#include "benchmarks.h"
#include "bench_harness.h"

#include "ts/access.h"
#include "ts/coroutine_support.h"
#include "ts/guarded.h"
#include "ts/scheduler.h"
#include "ts/static_task_graph.h"
#include "ts/task.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace
{

// Pipe contention: N producer threads hammer one object's reader/writer pipe with a
// read-heavy async mix (`read_pct`% reads). This is the fixture the pipe rebase must be
// validated non-regressive against (docs/internals/pipe-rebase.md R10): it stresses the current
// `Pipe::mutex` on every enqueue/admission - the cost the lock-free tail is meant to
// remove without hurting the uncontended path. 100% reads isolates admission throughput
// (readers never serialize); mixing writes adds serialization + the writer/reader handoff.
std::vector<double> bench_pipe_contention(unsigned producers, int read_pct)
{
    ts::Guarded<uint64_t> obj{ ts::Named{}, 0 };
    std::atomic<uint64_t> completed{ 0 };

    auto run_once = [&]() -> uint64_t
    {
        std::atomic<bool> stop{ false };
        std::atomic<uint64_t> submitted{ 0 };
        {
            std::vector<std::jthread> threads;
            for (unsigned i = 0; i < producers; ++i)
            {
                threads.emplace_back([&, i]
                {
                    uint64_t local = 0;
                    while (!stop.load(std::memory_order_relaxed))
                    {
                        // Deterministic per-thread read/write interleave (no RNG).
                        if (static_cast<int>((local + i) % 100) < read_pct)
                            obj.async([&completed](const uint64_t& v)
                                { completed.fetch_add(1, std::memory_order_relaxed); return v; });
                        else
                            obj.async([&completed](uint64_t& v)
                                { ++v; completed.fetch_add(1, std::memory_order_relaxed); });
                        ++local;
                    }
                    submitted.fetch_add(local, std::memory_order_relaxed);
                });
            }
            std::this_thread::sleep_for(target);
            stop.store(true, std::memory_order_relaxed);
        }   // join producers

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

// Guarded write: serialized async writes through one object's pipe.
std::vector<double> bench_ts_write()
{
    ts::Guarded<uint64_t> obj{ ts::Named{}, 0 };
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
    ts::Guarded<uint64_t> obj{ ts::Named{}, 7 };
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

// Bare task creation + boundary sync: the `make_executable` path (the fusion baseline).
std::vector<double> bench_launch_sync()
{
    return measure([&]() -> uint64_t
    {
        (void)ts::launch([] { return 1; }).sync();
        return 1;
    });
}

// Coroutine task creation + boundary sync: the fused frame+block path (one allocation,
// docs/internals/coroutine-first.md §5.1). Target: at or below `launch` above.
static ts::Task<int> trivial_coro() { co_return 1; }

std::vector<double> bench_coro_sync()
{
    return measure([&]() -> uint64_t
    {
        (void)trivial_coro().sync();
        return 1;
    });
}

// Coroutine composition: await a chain of K eagerly-launched stages - the coroutine-first
// equivalent of the `then` chain benchmark below (the number to beat, §5.5).
static ts::Task<int> chain_coro(int chain)
{
    int v = co_await ts::launch([] { return 0; });
    for (int k = 0; k < chain; ++k)
        v = co_await ts::launch([v] { return v + 1; });
    co_return v;
}

std::vector<double> bench_coro_chain()
{
    constexpr int chain = 50;
    return measure([&]() -> uint64_t
    {
        (void)chain_coro(chain).sync();
        return chain;
    });
}

// The decomposition partner of `coro chn`. Same shape - K awaited stages, one op per stage
// - but each stage is a plain coroutine call instead of a launched task. Tasks are eager,
// so the callee runs to completion on this thread and the await takes the `await_ready` fast
// path: this measures the per-stage coroutine cost (frame allocation + promise setup +
// settled-await + destruction) with no scheduler in the picture. `coro chn` minus this is
// the round-trip (submit, worker wake, cross-thread resume), which is what that benchmark is
// actually dominated by - see docs/internals/pipe-rebase.md §0.4.
static ts::Task<int> add_one_coro(int v) { co_return v + 1; }

static ts::Task<int> nest_coro(int chain)
{
    int v = 0;
    for (int k = 0; k < chain; ++k)
        v = co_await add_one_coro(v);
    co_return v;
}

std::vector<double> bench_coro_nest()
{
    constexpr int chain = 50;
    return measure([&]() -> uint64_t
    {
        (void)nest_coro(chain).sync();
        return chain;
    });
}

// The resume round trip, decomposed (TODO 6.8). `coro chn` minus `coro nst` is the per-stage
// round trip; this grid splits that round trip along its two structural axes by running the
// same chain on a differently-configured global scheduler (`ts::launch` dispatches through
// `global_scheduler()`, so `Scheduler_scope` is what redirects it):
//   idle policy - `spin` never parks a worker, so a submit issues no wake syscall and a
//                  waiting worker never sleeps; the delta against `spin_then_block` is the
//                  wake+park cost.
//   worker count - with one worker the stage runs and the awaiting frame resumes on the same
//                   thread (the resume trampoline, warm cache, own-deque submit); 2 workers
//                   adds exactly one possible thief, and the full pool adds the scan
//                   contention of a large idle pool.
// What remains at 1 worker + `spin`, above `coro nst`, is the queue round trip itself. The
// chain is strictly serial, so nothing here is paying for parallelism it gains: every stage
// is awaited before the next is launched.
std::vector<double> bench_coro_chain_on(ts::Scheduler_config config)
{
    ts::Scheduler_scope scope(config);
    return bench_coro_chain();
}

// Awaited join over 4 async reads (the coroutine-first form of the old typed join).
std::vector<double> bench_coro_join()
{
    ts::Guarded<int> a{ ts::Named{}, 1 }, b{ ts::Named{}, 2 }, c{ ts::Named{}, 3 }, d{ ts::Named{}, 4 };
    auto read = [](const int& v) { return v; };
    return measure([&]() -> uint64_t
    {
        (void)[&]() -> ts::Task<int>
        {
            ts::Task<int> ta = a.async(read), tb = b.async(read), tc = c.async(read), td = d.async(read);
            co_return co_await ta + co_await tb + co_await tc + co_await td;
        }().sync();
        return 1;
    });
}

// Static_task_graph: per-execute() dispatch + sync cost (8 independent nodes).
std::vector<double> bench_graph_execute()
{
    constexpr int nodes = 8;
    // An array of `Guarded` names each element: the type has no unnamed constructor, and
    // `Guarded` is neither copyable nor movable, so the elements are built in place from
    // one `ts::Named` each.
    std::array<ts::Guarded<int>, nodes> stores{ ts::Named{}, ts::Named{}, ts::Named{}, ts::Named{},
                                                ts::Named{}, ts::Named{}, ts::Named{}, ts::Named{} };
    ts::Static_task_graph g;
    for (auto& s : stores)
        g.add_node(ts::Named{}, [](int& v) { ++v; }, s);
    g.compile();
    return measure([&]() -> uint64_t
    {
        g.execute().sync();
        return 1;
    });
}

// Uncontended single-object floor: a read `access` + boundary `sync()` on a free pipe, one
// thread - the pure mechanism cost with zero contention (the Access_op acceptance metric,
// docs/internals/access-op-design.md §7 phase 0), reported beside the bare-mutex floor it is measured
// against.
std::vector<double> bench_access_floor()
{
    ts::Guarded<uint64_t> obj{ ts::Named{}, 7 };
    constexpr uint64_t batch = 20000;
    volatile uint64_t sink = 0;
    return measure([&]() -> uint64_t
    {
        uint64_t sum = 0;
        for (uint64_t i = 0; i < batch; ++i)
            sum += obj.access([](const uint64_t& v) { return v; }).sync();
        sink = sum;
        return batch;
    });
}

// The bare-mutex equivalent of the row above: lock, read, unlock - what a hand-rolled
// mutex-guarded object pays for the same uncontended access.
std::vector<double> bench_mutex_floor()
{
    std::mutex m;
    uint64_t value = 7;
    constexpr uint64_t batch = 20000;
    volatile uint64_t sink = 0;
    return measure([&]() -> uint64_t
    {
        uint64_t sum = 0;
        for (uint64_t i = 0; i < batch; ++i)
        {
            std::scoped_lock lock(m);
            sum += value;
        }
        sink = sum;
        return batch;
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

void run_features_bench()
{
    unsigned hw = std::thread::hardware_concurrency();

    std::printf("\npipe contention (%u producers, read-heavy mix - pipe rebase R10 baseline):\n", hw);
    report("100% rd", bench_pipe_contention(hw, 100));
    report("90% rd",  bench_pipe_contention(hw, 90));
    report("50% rd",  bench_pipe_contention(hw, 50));

    std::printf("\nfeatures:\n");
    report("launch", bench_launch_sync());
    report("coro", bench_coro_sync());
    report("coro chn", bench_coro_chain());
    report("coro nst", bench_coro_nest());
    report("ts_write", bench_ts_write());
    report("ts_read", bench_ts_read());
    report("access", bench_access_floor());
    report("mutex", bench_mutex_floor());
    report("coro join", bench_coro_join());
    report("graph", bench_graph_execute());
    report("harness", bench_harness());
}

void run_coro_resume_bench()
{
    std::printf("\ncoroutine resume round trip, decomposed (ns/op = ns per awaited stage):\n");
    report("N w s+blk", bench_coro_chain_on({ .idle_policy = ts::Idle_policy::spin_then_block }));
    report("N w spin", bench_coro_chain_on({ .idle_policy = ts::Idle_policy::spin }));
    report("1 w s+blk", bench_coro_chain_on({ .num_workers = 1, .idle_policy = ts::Idle_policy::spin_then_block }));
    report("1 w spin", bench_coro_chain_on({ .num_workers = 1, .idle_policy = ts::Idle_policy::spin }));
    report("2 w spin", bench_coro_chain_on({ .num_workers = 2, .idle_policy = ts::Idle_policy::spin }));
}
