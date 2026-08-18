#include "benchmarks.h"
#include "bench_harness.h"

#include "ts/coroutine_support.h"
#include "ts/guarded.h"
#include "ts/scheduler.h"
#include "ts/task.h"

#include <algorithm>
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

// --- subsystem locks: skewed multi-store contention, mixed task shape -----------------
//
// The realistic middle the earlier groups missed: not one saturated global (a granularity
// bug no mechanism survives) and not a synthetic job stream with no work affinity. Here a
// task looks like production task-parallel code written by a careful-but-not-heroic team:
// one lock per subsystem (16 stores, popularity skewed - one busy store around half
// utilization at core-bound throughput, a second warm, a long cool tail), critical
// sections kept small (80% ~150 ns, 20% ~1 us), private work between touches, and half
// the touches are reads whose value the task consumes before proceeding - the dependency
// fire-and-forget staging cannot fake. Contention is probabilistic (convoys and tail
// latency), not a hard ceiling; that is how the pain presents in the wild.
//
// Rows: `mtx hw`/`mtx 2x` run plain-function tasks, touches inline under `std::mutex`
// (a contended lock parks the worker; `2x` is the industry mitigation - backfill parked
// cores at context-switch cost). `grd hw` runs the same task as a coroutine: consumed
// reads are `co_await store.access(...)` (a contended touch suspends the frame and the
// worker takes another task), updates are fire-and-forget `access`. The coroutine frame
// and per-touch block allocations are part of the honest port cost and are counted.
// `grd 2x` is omitted - measured harmful in the global-bottleneck group. Both worlds get
// the identical pregenerated touch script and the same raw-submit flood.
//
// Reported per row: throughput, plus per-task service latency percentiles (start of body
// to completion, including lock waits / suspensions) - sub-saturated contention shows up
// in the tail long before it shows in throughput.

constexpr int store_count = 16;
constexpr int tasks_per_batch = 20000;
constexpr int touches_per_task = 2;
constexpr int private_segment_ns = 2000;   // x3 segments per task

struct Touch
{
    std::uint16_t store;
    std::uint16_t dur_ns;
    bool consumed_read;   // the task needs the value before proceeding
};

void spin_ns(int ns)
{
    if (ns <= 0)
        return;
    auto end = Clock::now() + std::chrono::nanoseconds(ns);
    while (Clock::now() < end)
    {
    }
}

// Skewed store popularity: weights[0]=6, then 3, 2, 2, and 1 for the cool tail
// (sum 25 - the busy store takes 24% of all touches). `uniform` levels them.
std::vector<Touch> make_script(bool uniform)
{
    int weights[store_count] = { 6, 3, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    if (uniform)
    {
        for (int& w : weights)
            w = 1;
    }
    int total_weight = 0;
    for (int w : weights)
        total_weight += w;

    std::vector<Touch> script(static_cast<std::size_t>(tasks_per_batch) * touches_per_task);
    std::uint64_t r = 0x2545f4914f6cdd1dull;
    for (Touch& t : script)
    {
        r = r * 6364136223846793005ull + 1442695040888963407ull;
        int pick = static_cast<int>((r >> 33) % static_cast<unsigned>(total_weight));
        int store = 0;
        while (pick >= weights[store])
        {
            pick -= weights[store];
            ++store;
        }
        t.store = static_cast<std::uint16_t>(store);
        t.dur_ns = static_cast<std::uint16_t>((r >> 13) % 100 < 80 ? 150 : 1000);
        t.consumed_read = ((r >> 7) & 1) != 0;
    }
    return script;
}

double avg_touch_ns(const std::vector<Touch>& script)
{
    double sum = 0.0;
    for (const Touch& t : script)
        sum += t.dur_ns;
    return sum / static_cast<double>(script.size());
}

struct alignas(64) Mutex_store
{
    std::mutex m;
    std::uint64_t v = 0;
};

struct Payload
{
    std::uint64_t v = 0;
};

// Per-task service latency (body start -> completion), recorded by claim id;
// percentiles over the last measured batch.
struct Latency_sink
{
    std::vector<std::uint32_t> ns;

    void print(const char* name)
    {
        std::vector<std::uint32_t> v = ns;
        auto pct = [&](double p) -> double
        {
            std::size_t k = static_cast<std::size_t>(p * (v.size() - 1));
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
            return v[k] / 1000.0;
        };
        std::printf("  %-10s task latency p50/p95/p99: %5.1f / %5.1f / %5.1f us\n",
            name, pct(0.50), pct(0.95), pct(0.99));
    }
};

struct Bench_ctx
{
    const std::vector<Touch>* script;
    std::atomic<int> claim{ 0 };
    std::atomic<int> done{ 0 };
    Latency_sink* lat;
    Mutex_store* mtx_stores;                                       // mutex rows
    std::vector<std::unique_ptr<ts::Guarded<Payload>>>* grd_stores; // guarded row
};

void mtx_task(void* p)
{
    auto* c = static_cast<Bench_ctx*>(p);
    int id = c->claim.fetch_add(1, std::memory_order_relaxed);
    auto t0 = Clock::now();
    std::uint64_t sink = 0;
    for (int k = 0; k < touches_per_task; ++k)
    {
        spin_ns(private_segment_ns);
        const Touch& t = (*c->script)[static_cast<std::size_t>(id) * touches_per_task + k];
        Mutex_store& s = c->mtx_stores[t.store];
        {
            std::scoped_lock lock(s.m);
            spin_ns(t.dur_ns);
            if (t.consumed_read)
                sink += s.v;   // value consumed by the rest of the task
            else
                ++s.v;
        }
    }
    spin_ns(private_segment_ns + static_cast<int>(sink % 2));   // consume `sink`
    c->lat->ns[static_cast<std::size_t>(id)] =
        static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    c->done.fetch_add(1, std::memory_order_release);
}

ts::Task<void> grd_task_coro(Bench_ctx* c, int id)
{
    auto t0 = Clock::now();
    std::uint64_t sink = 0;
    for (int k = 0; k < touches_per_task; ++k)
    {
        spin_ns(private_segment_ns);
        const Touch& t = (*c->script)[static_cast<std::size_t>(id) * touches_per_task + k];
        ts::Guarded<Payload>& s = *(*c->grd_stores)[t.store];
        int dur = t.dur_ns;
        if (t.consumed_read)
        {
            sink += co_await s.access([dur](const Payload& p)
            {
                spin_ns(dur);
                return p.v;
            });
        }
        else
        {
            (void)s.access([dur](Payload& p)
            {
                spin_ns(dur);
                ++p.v;
            });
        }
    }
    spin_ns(private_segment_ns + static_cast<int>(sink % 2));
    c->lat->ns[static_cast<std::size_t>(id)] =
        static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    c->done.fetch_add(1, std::memory_order_release);
}

void grd_task(void* p)
{
    auto* c = static_cast<Bench_ctx*>(p);
    int id = c->claim.fetch_add(1, std::memory_order_relaxed);
    (void)grd_task_coro(c, id);   // eager frame, self-owned once the handle drops
}

std::vector<double> bench_subsystem(bool guarded, unsigned workers, const std::vector<Touch>& script,
    Latency_sink& lat)
{
    ts::Scheduler_scope scope{ { .num_threads = workers } };
    ts::Scheduler& sched = ts::global_scheduler();

    auto mtx_stores = std::make_unique<Mutex_store[]>(store_count);
    std::vector<std::unique_ptr<ts::Guarded<Payload>>> grd_stores;
    for (int i = 0; i < store_count; ++i)
        grd_stores.push_back(std::make_unique<ts::Guarded<Payload>>(ts::Named{}));

    Bench_ctx ctx;
    ctx.script = &script;
    ctx.lat = &lat;
    ctx.mtx_stores = mtx_stores.get();
    ctx.grd_stores = &grd_stores;
    lat.ns.assign(tasks_per_batch, 0);

    return measure([&]() -> uint64_t
    {
        ctx.claim.store(0, std::memory_order_relaxed);
        ctx.done.store(0, std::memory_order_relaxed);
        for (int i = 0; i < tasks_per_batch; ++i)
            sched.submit(guarded ? &grd_task : &mtx_task, &ctx);
        while (ctx.done.load(std::memory_order_acquire) != tasks_per_batch)
            std::this_thread::yield();
        return tasks_per_batch;
    });
}

void run_scenario(const char* title, bool uniform, unsigned hw)
{
    std::vector<Touch> script = make_script(uniform);
    double touch_avg = avg_touch_ns(script);
    double task_ns = 3.0 * private_segment_ns + touches_per_task * touch_avg;
    double core_bound = hw * 1e3 / task_ns;                          // M tasks/s
    double hot_share = uniform ? 1.0 / store_count : 6.0 / 25.0;
    double hot_util = hot_share * touches_per_task * core_bound * 1e6 * touch_avg * 1e-9;
    std::printf("  %s (avg touch %.0f ns; core bound %.2f M tasks/s; busiest store ~%.0f%%"
                " utilized at that bound):\n", title, touch_avg, core_bound, hot_util * 100.0);

    Latency_sink lat;
    report("mtx hw", bench_subsystem(false, hw, script, lat));
    lat.print("mtx hw");
    report("mtx 2x", bench_subsystem(false, 2 * hw, script, lat));
    lat.print("mtx 2x");
    report("grd hw", bench_subsystem(true, hw, script, lat));
    lat.print("grd hw");
}

} // namespace

void run_subsystem_contention_bench()
{
    unsigned hw = std::thread::hardware_concurrency();
    std::printf("\nsubsystem locks (16 stores, %d touches/task, mixed 150ns/1us sections,"
                " consumed reads; %d tasks/batch):\n", touches_per_task, tasks_per_batch);
    run_scenario("skewed (6/3/2/2/1... weights)", false, hw);
    run_scenario("uniform (control)", true, hw);
}
