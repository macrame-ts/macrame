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

// Early release: an object touched only by a fast node is freed while a slow node (on a
// different object) still runs, so async on it runs *during* the run. Deterministic: the
// slow node, after its sleep, records whether the async already completed. Early release
// => the async ran within the 60ms window (flag set); the coarse "hold for the whole
// run" behavior would block the async until run end, so the flag would be unset.
void test_early_release_frees_object_mid_run()
{
    ts::Thread_safe<int> x{ 0 }, y{ 0 };
    std::atomic<bool> async_ran{ false };
    std::atomic<bool> ran_during_run{ false };

    ts::Static_task_graph g;
    g.add_node([](int& v) { v = 1; }, x);   // fast: sole accessor of x
    g.add_node([&async_ran, &ran_during_run](int& v)
    {
        std::this_thread::sleep_for(60ms);
        ran_during_run.store(async_ran.load());   // did x's async run while this node was still going?
        v = 1;
    }, y);   // slow: sole accessor of y
    g.compile();

    auto run = g.execute();
    ts::Task<void> as = x.async([&async_ran](int&) { async_ran.store(true); });
    run.get();
    as.get();

    TS_CHECK(async_ran.load());        // the async ran
    TS_CHECK(ran_during_run.load());   // and mid-run: x was freed early, not held to run end
}

// Lazy acquire: an object touched only by a LATE node (after a slow predecessor) is
// reserved only when that node is dispatched, so async on it runs during the early part
// of the frame instead of blocking on the whole run. Deterministic the same way: the
// slow predecessor (which does NOT touch x) records whether x's async already ran.
void test_lazy_acquire_late_object_free_early()
{
    ts::Thread_safe<int> y{ 0 }, x{ 0 };
    std::atomic<bool> async_ran{ false };
    std::atomic<bool> ran_during_a{ false };

    ts::Static_task_graph g;
    ts::Graph_node a = g.add_node([&async_ran, &ran_during_a](int& v)
    {
        std::this_thread::sleep_for(60ms);
        ran_during_a.store(async_ran.load());   // x's async ran while a (no x access) was active?
        v = 1;
    }, y);
    ts::Graph_node b = g.add_node([](int& v) { v = 1; }, x);   // x touched only after a
    b.after(a);
    g.compile();

    auto run = g.execute();
    ts::Task<void> as = x.async([&async_ran](int&) { async_ran.store(true); });
    run.get();
    as.get();

    TS_CHECK(async_ran.load());
    TS_CHECK(ran_during_a.load());   // x was free while the 60ms node ran (reserved only when b dispatched)
}

// Gap freeing (per-node acquire, not whole-run): an object touched by an EARLY node and a
// LATE node, with a gap node (on another object) between them, is RELEASED after the early
// node and re-acquired only at the late node -- so it is FREE during the gap. The old
// whole-run [first accessor, last accessor] reservation held it continuously across the gap,
// which would leave `ran_during_gap` false. Deterministic: the gap node (touches only y)
// sleeps and records whether x's async already ran.
void test_gap_frees_object_between_accessors()
{
    ts::Thread_safe<int> x{ 0 }, y{ 0 };
    std::atomic<bool> async_ran{ false };
    std::atomic<bool> ran_during_gap{ false };

    ts::Static_task_graph g;
    ts::Graph_node n1 = g.add_node([](int& v) { v = 1; }, x);   // early accessor of x
    ts::Graph_node n2 = g.add_node([&async_ran, &ran_during_gap](int& v)
    {
        std::this_thread::sleep_for(60ms);
        ran_during_gap.store(async_ran.load());   // x's async ran while the gap node was active?
        v = 1;
    }, y);                                                      // gap: touches only y
    ts::Graph_node n3 = g.add_node([](int& v) { v = 2; }, x);   // late accessor of x
    n2.after(n1);
    n3.after(n2);   // order n1 -> n2 -> n3 (n1 -> n3 is also an auto x-conflict edge)
    g.compile();

    auto run = g.execute();
    ts::Task<void> as = x.async([&async_ran](int&) { async_ran.store(true); });
    run.get();
    as.get();

    TS_CHECK(async_ran.load());
    TS_CHECK(ran_during_gap.load());   // x released after n1, re-acquired at n3 -> free during n2
}

// Mode-aware acquire: a READ node holds its object as a reader, so a concurrent async READ
// on the same object overlaps it. The old whole-run reservation was writer-exclusive and
// blocked any async (even a read) for the run; per-node mode-aware acquire lets concurrent
// reads run. Deterministic: the read node sleeps, then records whether the async read ran.
void test_reader_node_overlaps_async_read()
{
    ts::Thread_safe<int> x{ 7 };
    std::atomic<bool> async_ran{ false };
    std::atomic<bool> ran_during_node{ false };

    ts::Static_task_graph g;
    g.add_node([&async_ran, &ran_during_node](const int& v)
    {
        std::this_thread::sleep_for(60ms);
        ran_during_node.store(async_ran.load());
        (void)v;
    }, x);   // READ node (const ref -> read_only, held as a reader)
    g.compile();

    auto run = g.execute();
    ts::Task<int> as = x.async([&async_ran](const int& v) { async_ran.store(true); return v; });   // READ async
    run.get();
    TS_CHECK(as.get() == 7);

    TS_CHECK(async_ran.load());
    TS_CHECK(ran_during_node.load());   // read async overlapped the read node (both readers)
}

// J: repeat a concurrency-sensitive workload to catch flakiness. Each iteration uses a
// deterministic gate (two readers wait for each other) rather than a hoped "peak > 1".
void test_repeat_stress()
{
    bool all = true;
    for (int iter = 0; iter < 20; ++iter)
    {
        tests::Parallel_gate gate{ 2 };
        ts::Thread_safe<int> data{ 0 };
        std::vector<ts::Task<int>> tasks;

        for (int i = 0; i < 8; ++i)
            tasks.push_back(data.async([&gate](const int& v) { gate.arrive(); return v; }));
        for (auto& t : tasks)
            t.get();

        all = all && gate.met();
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

// Deep retraction through a `then` chain: each outer task get()s a CONTINUATION, whose
// completion is continuation-driven (an attach callback), not lock-counter driven. The
// retraction-hint backlink lets the blocked get() walk to the producer and run it inline,
// so a then chain is deadlock-free under oversubscription like an after chain.
void test_then_retraction_no_deadlock()
{
    std::atomic<int> total{ 0 };
    std::atomic<bool> done{ false };
    const int outer = 2 * static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    std::thread runner([&]
    {
        naive_parallel_for(outer, [&](int)
        {
            int r = ts::launch([&] { total.fetch_add(1); return 20; })   // retractable producer
                        .then([&](int x) { total.fetch_add(1); return x + 1; })
                        .get();   // deep-retract: run the producer inline, its callback fires the continuation
            (void)r;
        });
        done.store(true);
    });

    for (int i = 0; i < 300 && !done.load(); ++i)
        std::this_thread::sleep_for(10ms);

    TS_CHECK(done.load());   // false => `then` not deep-retractable
    if (done.load())
    {
        runner.join();
        TS_CHECK(total.load() == outer * 2);   // producer + continuation, once per outer
    }
    else
        runner.detach();
}

// Same, through a when_all join: get() on the join walks its retraction hints, runs each
// (retractable) prerequisite inline, they settle -> finish completes the join.
void test_when_all_retraction_no_deadlock()
{
    std::atomic<int> total{ 0 };
    std::atomic<bool> done{ false };
    const int outer = 2 * static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    std::thread runner([&]
    {
        naive_parallel_for(outer, [&](int)
        {
            ts::Task<int> a = ts::launch([&] { total.fetch_add(1); return 1; });
            ts::Task<int> b = ts::launch([&] { total.fetch_add(1); return 2; });
            int s = ts::when_all(a, b).then([](int x, int y) { return x + y; }).get();
            (void)s;
        });
        done.store(true);
    });

    for (int i = 0; i < 300 && !done.load(); ++i)
        std::this_thread::sleep_for(10ms);

    TS_CHECK(done.load());   // false => when_all not deep-retractable
    if (done.load())
    {
        runner.join();
        TS_CHECK(total.load() == outer * 2);
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
    run("gap frees object between accessors", test_gap_frees_object_between_accessors);
    run("reader node overlaps async read", test_reader_node_overlaps_async_read);
    run("repeat stress x20", test_repeat_stress);
    run("engine frame invariants", test_engine_frame);
    run("engine determinism", test_engine_determinism);
    run("oversubscription no deadlock", test_oversubscription_no_deadlock);
    run("deep retraction no deadlock", test_deep_retraction_no_deadlock);
    run("then retraction no deadlock", test_then_retraction_no_deadlock);
    run("when_all retraction no deadlock", test_when_all_retraction_no_deadlock);
}
