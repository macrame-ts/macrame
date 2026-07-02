#include "integration_tests.h"
#include "thread_safe.h"
#include "static_task_graph.h"
#include "harness.h"
#include "test_util.h"
#include "engine.h"

#include <algorithm>
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

// Early release: an object touched only by a fast node is freed while a slow node
// (on a different object) still runs, so async on it runs well before run completion.
void test_early_release_frees_object_mid_run()
{
    using clock = std::chrono::steady_clock;
    auto ms_since = [](clock::time_point t0)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
    };

    ts::Thread_safe<int> x{ 0 }, y{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { v = 1; }, x);                                    // fast: sole accessor of x
    g.add_node([](int& v) { std::this_thread::sleep_for(60ms); v = 1; }, y); // slow: sole accessor of y
    g.compile();

    auto t0 = clock::now();
    auto run = g.execute();
    std::atomic<long long> x_async_ms{ -1 };
    x.async([&](int&) { x_async_ms.store(ms_since(t0)); });
    run.get();
    long long run_ms = ms_since(t0);

    TS_CHECK(x_async_ms.load() >= 0);              // the async ran
    TS_CHECK(x_async_ms.load() + 20 < run_ms);     // and well before the 60ms run finished
}

// Lazy acquire: an object touched only by a LATE node (after a slow predecessor) is
// reserved only when that node is dispatched, so async on it runs during the early
// part of the frame instead of blocking on the whole run.
void test_lazy_acquire_late_object_free_early()
{
    using clock = std::chrono::steady_clock;
    auto ms_since = [](clock::time_point t0)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
    };

    ts::Thread_safe<int> y{ 0 }, x{ 0 };
    ts::Static_task_graph g;
    ts::Graph_node a = g.add_node([](int& v) { std::this_thread::sleep_for(60ms); v = 1; }, y);
    ts::Graph_node b = g.add_node([](int& v) { v = 1; }, x);   // x touched only after a
    b.after(a);
    g.compile();

    auto t0 = clock::now();
    auto run = g.execute();
    std::atomic<long long> x_async_ms{ -1 };
    x.async([&](int&) { x_async_ms.store(ms_since(t0)); });
    run.get();
    long long run_ms = ms_since(t0);

    TS_CHECK(x_async_ms.load() >= 0);
    TS_CHECK(x_async_ms.load() + 30 < run_ms);     // x was free while the 60ms node ran
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

// A naive blocking fork-join: launch a task per item, then get() each.
template<typename Fn>
void naive_parallel_for(int n, Fn fn)
{
    std::vector<ts::Task<void>> tasks;
    for (int i = 0; i < n; ++i)
        tasks.push_back(ts::launch([fn, i] { fn(i); }));
    for (auto& t : tasks)
        t.get();
}

// Nested parallel-for → oversubscription deadlock. The outer tasks saturate every
// worker and each blocks in a get() waiting on its inner tasks; the inner tasks sit in
// the queue with no free worker to run them → classic deadlock. Retraction breaks it:
// a blocked get() runs the un-started task inline on the waiting thread instead of
// parking. Watchdog'd so a deadlock fails the test rather than hanging forever (this
// test runs last, so a poisoned scheduler doesn't affect the others).
void test_oversubscription_no_deadlock()
{
    std::atomic<int> total{ 0 };
    std::atomic<bool> done{ false };
    const int outer = 2 * static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    std::thread runner([&]
    {
        naive_parallel_for(outer, [&](int)
        {
            naive_parallel_for(4, [&](int) { total.fetch_add(1); });
        });
        done.store(true);
    });

    for (int i = 0; i < 300 && !done.load(); ++i)
        std::this_thread::sleep_for(10ms);   // up to ~3s

    TS_CHECK(done.load());   // false => oversubscription deadlock (retraction not working)
    if (done.load())
    {
        runner.join();
        TS_CHECK(total.load() == outer * 4);
    }
    else
        runner.detach();     // deadlocked; leak the stuck thread
}

// Deep retraction: each outer task get()s a DEPENDENT (a builder task with
// prerequisites), not the leaf chunks. Simple retraction can't run it (its
// prerequisites aren't met); deep retraction walks its prerequisites, runs the
// un-started chunks inline, then runs the dependent — so it too avoids the
// oversubscription deadlock.
void test_deep_retraction_no_deadlock()
{
    std::atomic<int> total{ 0 };
    std::atomic<bool> done{ false };
    const int outer = 2 * static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    std::thread runner([&]
    {
        naive_parallel_for(outer, [&](int)
        {
            ts::Task<void> a = ts::launch([&] { total.fetch_add(1); });
            ts::Task<void> b = ts::launch([&] { total.fetch_add(1); });
            ts::Task<void> c = ts::launch([&] { total.fetch_add(1); });
            ts::Task<void> join = ts::task([] {}).after(a, b, c).launch();
            join.get();   // deep-retract: run a/b/c inline, then the join
        });
        done.store(true);
    });

    for (int i = 0; i < 300 && !done.load(); ++i)
        std::this_thread::sleep_for(10ms);

    TS_CHECK(done.load());   // false => deep retraction not working
    if (done.load())
    {
        runner.join();
        TS_CHECK(total.load() == outer * 3);
    }
    else
        runner.detach();
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
    run("early release frees object mid-run", test_early_release_frees_object_mid_run);
    run("lazy acquire keeps late object free", test_lazy_acquire_late_object_free_early);
    run("repeat stress x20", test_repeat_stress);
    run("engine frame invariants", test_engine_frame);
    run("engine determinism", test_engine_determinism);
    run("oversubscription no deadlock", test_oversubscription_no_deadlock);
    run("deep retraction no deadlock", test_deep_retraction_no_deadlock);
}
