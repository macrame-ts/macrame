#include "benchmarks.h"
#include "ts/scheduler.h"
#include "ts/guarded.h"
#include "ts/static_task_graph.h"
#include "ts/access.h"

#include "ts/coroutine_support.h"

// The game-frame sample is a single self-contained .cpp (no header); the two compositions
// this benchmark compares are forward-declared. Same World, same system bodies: one
// scheduled by a compiled `Static_task_graph`, one hand-composed with coroutines.
namespace sample
{
void game_frame_stats(int frames, float time_scale, double& avg_ms, double& serial_ms, float& transform0);
void game_frame_free_stats(int frames, float time_scale, double& avg_ms, double& serial_ms, float& transform0);
}

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

// --- public feature benchmarks --------------------------------------------

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

// Pipe contention: N producer threads hammer ONE object's reader/writer pipe with a
// read-heavy async mix (`read_pct`% reads). This is the fixture the pipe rebase must be
// validated non-regressive against (docs/pipe-rebase.md R10): it stresses the current
// `Pipe::mutex` on every enqueue/admission -- the cost the lock-free tail is meant to
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
// docs/coroutine-first.md §5.1). Target: at or below `launch` above.
static ts::Task<int> trivial_coro() { co_return 1; }

std::vector<double> bench_coro_sync()
{
    return measure([&]() -> uint64_t
    {
        (void)trivial_coro().sync();
        return 1;
    });
}

// Coroutine composition: await a chain of K eagerly-launched stages -- the coroutine-first
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

// The decomposition partner of `coro chn`. Same shape -- K awaited stages, one op per stage
// -- but each stage is a plain coroutine call instead of a launched task. Tasks are eager,
// so the callee runs to completion on this thread and the await takes the `await_ready` fast
// path: this measures the per-stage COROUTINE cost (frame allocation + promise setup +
// settled-await + destruction) with no scheduler in the picture. `coro chn` minus this is
// the round-trip (submit, worker wake, cross-thread resume), which is what that benchmark is
// actually dominated by -- see docs/pipe-rebase.md §0.4.
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
// SAME chain on a differently-configured global scheduler (`ts::launch` dispatches through
// `global_scheduler()`, so `Scheduler_scope` is what redirects it):
//   idle policy -- `spin` never parks a worker, so a submit issues no wake syscall and a
//                  waiting worker never sleeps; the delta against `spin_then_block` is the
//                  wake+park cost.
//   worker count -- with ONE worker the stage runs and the awaiting frame resumes on the same
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

// --- graph vs graph-free frame composition ---------------------------------
// Both entries run the SAME frame -- same `World`, same tick_* bodies, same worker pool
// (the global scheduler) -- differing only in how the schedule is produced: a compiled
// `Static_task_graph` (edges derived from the access declarations, re-runs allocate only
// the `done` handle) vs `run_frame_graph_free` (a coroutine per chain, one task block +
// pipe-link set per system per frame, every edge an explicit `co_await`). The graph's
// structural advantage is allocation amortization, so the interesting question is at what
// system granularity that becomes visible: `scale` shrinks every system's mock cost, and
// the fixed per-frame composition cost stays put.

using Frame_run = void (*)(int, float, double&, double&, float&);

double frame_us(Frame_run run, int frames, float scale)
{
    double avg_ms = 0.0, serial_ms = 0.0;
    float transform0 = 0.0f;
    run(frames, scale, avg_ms, serial_ms, transform0);   // warmup (also pages in the World)
    std::vector<double> us;
    for (int r = 0; r < reps; ++r)
    {
        run(frames, scale, avg_ms, serial_ms, transform0);
        us.push_back(avg_ms * 1000.0);
    }
    return median(std::move(us));
}

void report_frame(const char* name, double us_per_frame, double reference)
{
    if (reference > 0.0)
    {
        std::printf("  %-10s %9.1f us/frame   [%+6.1f%% vs graph]\n", name, us_per_frame,
            100.0 * (us_per_frame - reference) / reference);
    }
    else
    {
        std::printf("  %-10s %9.1f us/frame\n", name, us_per_frame);
    }
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

    std::printf("\npipe contention (%u producers, read-heavy mix -- pipe rebase R10 baseline):\n", hw);
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
    report("coro join", bench_coro_join());
    report("graph", bench_graph_execute());
    report("harness", bench_harness());

    std::printf("\ncoroutine resume round trip, decomposed (ns/op = ns per awaited stage):\n");
    report("N w s+blk", bench_coro_chain_on({ .idle_policy = ts::Idle_policy::spin_then_block }));
    report("N w spin", bench_coro_chain_on({ .idle_policy = ts::Idle_policy::spin }));
    report("1 w s+blk", bench_coro_chain_on({ .num_threads = 1, .idle_policy = ts::Idle_policy::spin_then_block }));
    report("1 w spin", bench_coro_chain_on({ .num_threads = 1, .idle_policy = ts::Idle_policy::spin }));
    report("2 w spin", bench_coro_chain_on({ .num_threads = 2, .idle_policy = ts::Idle_policy::spin }));

    std::printf("\ngame frame, graph vs graph-free (same World, same system bodies, %u hw threads):\n", hw);
    double heavy_graph = frame_us(&sample::game_frame_stats, 20, 1.0f);
    double heavy_free = frame_us(&sample::game_frame_free_stats, 20, 1.0f);
    double light_graph = frame_us(&sample::game_frame_stats, 200, 0.05f);
    double light_free = frame_us(&sample::game_frame_free_stats, 200, 0.05f);
    report_frame("graph 1.0", heavy_graph, 0.0);
    report_frame("free 1.0", heavy_free, heavy_graph);
    report_frame("graph .05", light_graph, 0.0);
    report_frame("free .05", light_free, light_graph);
}
