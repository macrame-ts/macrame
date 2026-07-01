#include "integration_tests.h"
#include "thread_safe.h"
#include "static_task_graph.h"
#include "harness.h"
#include "test_util.h"
#include "engine.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <tuple>
#include <vector>

using namespace std::chrono_literals;
using ts::test::run;
using tests::record_max;

namespace
{

int read_value(ts::Thread_safe<int>& d)
{
    return d.async([](const int& v) { return v; }).get();
}

// `then()` chained off the graph's `execute()` completion handle.
void test_then_off_graph_completion()
{
    ts::Thread_safe<int> a{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { v = 5; }, a);
    g.compile();

    std::atomic<bool> after{ false };
    g.execute().then([&after] { after.store(true); }).get();

    TS_CHECK(after.load());
    TS_CHECK(read_value(a) == 5);
}

// `when_all` over async results, feeding a value into a graph run.
void test_when_all_into_graph()
{
    ts::Thread_safe<int> a{ 2 }, b{ 3 };
    int sum = ts::when_all(
            a.async([](const int& v) { return v; }),
            b.async([](const int& v) { return v; }))
        .then([](std::tuple<int, int>& r) { return std::get<0>(r) + std::get<1>(r); })
        .get();

    ts::Thread_safe<int> c{ 0 };
    ts::Static_task_graph g;
    g.add_node([sum](int& v) { v = sum; }, c);
    g.compile();
    g.execute().get();

    TS_CHECK(read_value(c) == 5);
}

// graph run, then a dynamic async on the same object (sequential, no race).
void test_graph_then_dynamic()
{
    ts::Thread_safe<int> a{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { v = 7; }, a);
    g.compile();
    g.execute().get();

    TS_CHECK(read_value(a) == 7);
}

// Detects overlapping access: any two touches running at once push `peak` to 2.
// (The access harness would NOT catch a graph-node vs. async race -- both sides hold
// a valid declared context; only the reservation keeps them apart. So we measure
// concurrency directly.)
struct Guarded
{
    std::atomic<int> live{ 0 };
    std::atomic<int> peak{ 0 };
    std::atomic<int> total{ 0 };

    void touch()
    {
        record_max(peak, live.fetch_add(1) + 1);
        std::this_thread::sleep_for(100us);
        total.fetch_add(1);
        live.fetch_sub(1);
    }
};

int peak_of(ts::Thread_safe<Guarded>& x) { return x.async([](const Guarded& g) { return g.peak.load(); }).get(); }
int total_of(ts::Thread_safe<Guarded>& x) { return x.async([](const Guarded& g) { return g.total.load(); }).get(); }

// Static node access + dynamic async on the SAME object must not overlap: the run
// reserves the object, so the asyncs queue behind the node. (`execute()` reserves
// synchronously here -- the pipe is idle -- so the asyncs are enqueued behind the
// held reservation.)
void test_graph_async_no_overlap_during()
{
    ts::Thread_safe<Guarded> x;
    ts::Static_task_graph g;
    g.add_node([](Guarded& gg) { gg.touch(); }, x);
    g.compile();

    auto run = g.execute();
    std::vector<ts::Task<void>> asyncs;
    for (int i = 0; i < 4; ++i)
        asyncs.push_back(x.async([](Guarded& gg) { gg.touch(); }));
    run.get();
    for (auto& a : asyncs)
        a.get();

    TS_CHECK(peak_of(x) == 1);        // never concurrent
    TS_CHECK(total_of(x) == 5);       // node + 4 asyncs all ran
}

// A pending async before the run: the reservation is taken via the deferred path
// (pipe not idle) and waits behind the async; the node runs after it.
void test_async_before_graph_no_overlap()
{
    ts::Thread_safe<Guarded> x;
    auto pending = x.async([](Guarded& gg) { gg.touch(); });

    ts::Static_task_graph g;
    g.add_node([](Guarded& gg) { gg.touch(); }, x);
    g.compile();
    g.execute().get();
    pending.get();

    TS_CHECK(peak_of(x) == 1);
    TS_CHECK(total_of(x) == 2);
}

// Contention: threads hammer async on an object while the graph re-runs. A broken
// reservation would let a node and an async overlap -> peak == 2.
void test_graph_async_stress()
{
    ts::Thread_safe<Guarded> x;
    ts::Static_task_graph g;
    g.add_node([](Guarded& gg) { gg.touch(); }, x);
    g.compile();

    std::atomic<bool> stop{ false };
    {
        std::vector<std::jthread> firers;
        for (int t = 0; t < 4; ++t)
            firers.emplace_back([&]
            {
                while (!stop.load(std::memory_order_relaxed))
                    x.async([](Guarded& gg) { gg.touch(); }).get();
            });

        for (int i = 0; i < 20; ++i)
            g.execute().get();

        stop.store(true, std::memory_order_relaxed);
    }   // join firers

    TS_CHECK(peak_of(x) == 1);
}

// J: repeat a concurrency-sensitive workload to catch flakiness.
void test_repeat_stress()
{
    bool all = true;
    for (int iter = 0; iter < 20; ++iter)
    {
        std::atomic<int> active{ 0 }, peak{ 0 };
        ts::Thread_safe<int> data{ 0 };
        std::vector<ts::Task<int>> tasks;

        for (int i = 0; i < 8; ++i)
            tasks.push_back(data.async([&active, &peak](const int& v)
            {
                record_max(peak, active.fetch_add(1) + 1);
                std::this_thread::sleep_for(1ms);
                active.fetch_sub(1);
                return v;
            }));
        for (auto& t : tasks)
            t.get();

        all = all && (peak.load() > 1);
    }
    TS_CHECK(all);
}

// Drive the whole mock engine (graph + internal parallelism + Thread_safe::async
// + then/when_all) and assert frame-level invariants. Reaching the assertions at
// all proves no deadlock and -- since the access harness is live -- zero access
// violations (a violation would have aborted the process).
void test_engine_frame()
{
    sample::Frame_stats s = sample::run_frames(10, 1.0f);

    TS_CHECK(s.frames == 10);                    // every frame completed
    TS_CHECK(s.world_xf_value == 5.0f);          // deterministic, correct output (2 + 3)
    TS_CHECK(s.avg_ms * 1.3 < s.serial_ms);      // the graph actually parallelized (>1.3x)
}

void test_engine_determinism()
{
    sample::Frame_stats a = sample::run_frames(5, 0.3f);
    sample::Frame_stats b = sample::run_frames(5, 0.3f);
    TS_CHECK(a.world_xf_value == b.world_xf_value);
}

} // namespace

void run_integration_tests()
{
    std::printf("\n[integration] tests\n");
    run("then off graph completion", test_then_off_graph_completion);
    run("when_all into graph", test_when_all_into_graph);
    run("graph then dynamic", test_graph_then_dynamic);
    run("graph/async no overlap (during)", test_graph_async_no_overlap_during);
    run("graph/async no overlap (before)", test_async_before_graph_no_overlap);
    run("graph/async contention", test_graph_async_stress);
    run("repeat stress x20", test_repeat_stress);
    run("engine frame invariants", test_engine_frame);
    run("engine determinism", test_engine_determinism);
}
