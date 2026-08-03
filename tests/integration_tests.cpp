#include "integration_tests.h"
#include "ts/coroutine_support.h"
#include "ts/guarded.h"
#include "ts/parallel_for.h"
#include "ts/static_task_graph.h"
#include "harness.h"
#include "test_util.h"

// The game-frame sample is a single self-contained .cpp (no header); its test
// surface is forward-declared.
namespace sample
{
void game_frame_stats(int frames, float time_scale,
                      double& avg_ms, double& serial_ms, float& transform0);
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <tuple>
#include <vector>

using namespace std::chrono_literals;
using ts::test::run;
using tests::record_max;
using tests::wait_until;

namespace
{

int read_value(ts::Guarded<int>& d)
{
    return d.async([](const int& v) { return v; }).sync();
}

// The graph's completion handle awaited from a coroutine -- the coroutine-first spelling
// of the old `then`-off-`execute()` chain.
void test_await_graph_completion()
{
    ts::Guarded<int> a{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { v = 5; }, a);
    g.compile();

    std::atomic<bool> after{ false };
    [](ts::Static_task_graph& graph, std::atomic<bool>& flag) -> ts::Task<void>
    {
        co_await graph.execute();
        flag.store(true);
    }(g, after).sync();

    TS_CHECK(after.load());
    TS_CHECK(read_value(a) == 5);
}

// Joining async results into a graph run -- the coroutine-first spelling of the old
// `when_all(...).then(...)` join: the accesses run concurrently (eager), sequential awaits
// complete when the last does.
void test_await_join_into_graph()
{
    ts::Guarded<int> a{ 2 }, b{ 3 };
    int sum = [](ts::Guarded<int>& ga, ts::Guarded<int>& gb) -> ts::Task<int>
    {
        ts::Task<int> ra = ga.async([](const int& v) { return v; });
        ts::Task<int> rb = gb.async([](const int& v) { return v; });
        co_return co_await ra + co_await rb;
    }(a, b).sync();

    ts::Guarded<int> c{ 0 };
    ts::Static_task_graph g;
    g.add_node([sum](int& v) { v = sum; }, c);
    g.compile();
    g.execute().sync();

    TS_CHECK(read_value(c) == 5);
}

// graph run, then a dynamic async on the same object (sequential, no race).
void test_graph_then_dynamic()
{
    ts::Guarded<int> a{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { v = 7; }, a);
    g.compile();
    g.execute().sync();

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

int peak_of(ts::Guarded<Guarded>& x) { return x.async([](const Guarded& g) { return g.peak.load(); }).sync(); }
int total_of(ts::Guarded<Guarded>& x) { return x.async([](const Guarded& g) { return g.total.load(); }).sync(); }

// Static node access + dynamic async on the SAME object must not overlap: the run
// reserves the object, so the asyncs queue behind the node. (`execute()` reserves
// synchronously here -- the pipe is idle -- so the asyncs are enqueued behind the
// held reservation.)
void test_graph_async_no_overlap_during()
{
    ts::Guarded<Guarded> x;
    ts::Static_task_graph g;
    g.add_node([](Guarded& gg) { gg.touch(); }, x);
    g.compile();

    auto run = g.execute();
    std::vector<ts::Task<void>> asyncs;
    for (int i = 0; i < 4; ++i)
        asyncs.push_back(x.async([](Guarded& gg) { gg.touch(); }));
    run.sync();
    for (auto& a : asyncs)
        a.sync();

    TS_CHECK(peak_of(x) == 1);        // never concurrent
    TS_CHECK(total_of(x) == 5);       // node + 4 asyncs all ran
}

// A pending async before the run: the reservation is taken via the deferred path
// (pipe not idle) and waits behind the async; the node runs after it.
void test_async_before_graph_no_overlap()
{
    ts::Guarded<Guarded> x;
    auto pending = x.async([](Guarded& gg) { gg.touch(); });

    ts::Static_task_graph g;
    g.add_node([](Guarded& gg) { gg.touch(); }, x);
    g.compile();
    g.execute().sync();
    pending.sync();

    TS_CHECK(peak_of(x) == 1);
    TS_CHECK(total_of(x) == 2);
}

// Contention: threads hammer async on an object while the graph re-runs. A broken
// reservation would let a node and an async overlap -> peak == 2.
void test_graph_async_stress()
{
    ts::Guarded<Guarded> x;
    ts::Static_task_graph g;
    g.add_node([](Guarded& gg) { gg.touch(); }, x);
    g.compile();

    std::atomic<bool> stop{ false };
    {
        std::vector<std::jthread> firers;
        for (int t = 0; t < 4; ++t)
        {
            firers.emplace_back([&]
            {
                while (!stop.load(std::memory_order_relaxed))
                    x.async([](Guarded& gg) { gg.touch(); }).sync();
            });
        }

        for (int i = 0; i < 20; ++i)
            g.execute().sync();

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
    ts::Guarded<int> x{ 0 }, y{ 0 };
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
    run.sync();
    as.sync();

    TS_CHECK(async_ran.load());        // the async ran
    TS_CHECK(ran_during_run.load());   // and mid-run: x was freed early, not held to run end
}

// Lazy acquire: an object touched only by a LATE node (after a slow predecessor) is
// reserved only when that node is dispatched, so async on it runs during the early part
// of the frame instead of blocking on the whole run. Deterministic the same way: the
// slow predecessor (which does NOT touch x) records whether x's async already ran.
void test_lazy_acquire_late_object_free_early()
{
    ts::Guarded<int> y{ 0 }, x{ 0 };
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
    run.sync();
    as.sync();

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
    ts::Guarded<int> x{ 0 }, y{ 0 };
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
    run.sync();
    as.sync();

    TS_CHECK(async_ran.load());
    TS_CHECK(ran_during_gap.load());   // x released after n1, re-acquired at n3 -> free during n2
}

// Mode-aware acquire: a READ node holds its object as a reader, so a concurrent async READ
// on the same object overlaps it (a writer hold is exclusive; a whole-object reservation
// would block even reads for the whole run). Deterministic: the read node sleeps, then
// records whether the async read ran.
void test_reader_node_overlaps_async_read()
{
    ts::Guarded<int> x{ 7 };
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
    run.sync();
    TS_CHECK(as.sync() == 7);

    TS_CHECK(async_ran.load());
    TS_CHECK(ran_during_node.load());   // read async overlapped the read node (both readers)
}

// Multi-object async: run over several objects at once (per-arg mode from const-ness),
// holding all their pipes for the body. Basic correctness (write one, read another).
void test_multi_async_basic()
{
    ts::Guarded<int> a{ 10 }, b{ 20 };
    int result = ts::async([](int& x, const int& y) { x += y; return x; }, a, b).sync();   // a += b
    TS_CHECK(result == 30);
    TS_CHECK(read_value(a) == 30);
    TS_CHECK(read_value(b) == 20);
}

// A multi-object writer holds both objects exclusively, so single-object asyncs on either
// queue behind it -- never concurrent on either object.
void test_multi_async_exclusion()
{
    ts::Guarded<Guarded> x, y;
    std::vector<ts::Task<void>> tasks;
    for (int i = 0; i < 4; ++i)
    {
        tasks.push_back(ts::async([](Guarded& a, Guarded& b) { a.touch(); b.touch(); }, x, y));
        tasks.push_back(x.async([](Guarded& g) { g.touch(); }));
        tasks.push_back(y.async([](Guarded& g) { g.touch(); }));
    }
    for (auto& t : tasks)
        t.sync();
    TS_CHECK(peak_of(x) == 1);   // never concurrent on x
    TS_CHECK(peak_of(y) == 1);   // never concurrent on y
}

// Deadlock-freedom: two multi-object asyncs declaring the SAME pair in OPPOSITE order both
// acquire in canonical (pipe-address) order, so no hold-and-wait cycle forms. All complete.
void test_multi_async_no_deadlock()
{
    ts::Guarded<int> a{ 0 }, b{ 0 };
    std::vector<ts::Task<void>> tasks;
    for (int i = 0; i < 50; ++i)
    {
        tasks.push_back(ts::async([](int& x, int& y) { ++x; ++y; }, a, b));   // declared order a, b
        tasks.push_back(ts::async([](int& x, int& y) { ++x; ++y; }, b, a));   // declared order b, a
    }
    for (auto& t : tasks)
        t.sync();
    TS_CHECK(read_value(a) == 100);   // 100 tasks each incremented both
    TS_CHECK(read_value(b) == 100);
}

// Options (first arg): priority + token skip.
void test_multi_async_options()
{
    ts::Guarded<int> a{ 1 }, b{ 2 };
    int r = ts::async({ .priority = ts::Priority::high }, [](const int& x, const int& y) { return x + y; }, a, b).sync();
    TS_CHECK(r == 3);

    ts::Cancellation_source src;
    src.request_cancel();
    ts::Task<int> t = ts::async({ .token = src.token() }, [](const int& x, const int& y) { return x + y; }, a, b);
    wait_until([&] { return t.is_done(); });
    TS_CHECK(t.is_cancelled());   // skipped before running; don't get() a cancelled value
}

// Multi-object async with a GENERIC lambda: `[](auto&...)` can't be introspected for const-ness,
// so mode is declared with `ts::as_read_only`/`as_read_write` tags. Same effect as the non-generic
// `test_multi_async_basic` (write one, read the other).
void test_multi_async_generic_lambda()
{
    ts::Guarded<int> a{ 10 }, b{ 20 };
    int result = ts::async([](auto& x, const auto& y) { x += y; return x; },
                           ts::as_read_write(a), ts::as_read_only(b)).sync();   // a += b
    TS_CHECK(result == 30);
    TS_CHECK(read_value(a) == 30);
    TS_CHECK(read_value(b) == 20);

    // `ts::access` takes the same tags.
    int r2 = ts::access([](auto& x, auto& y) { return x + y; },
                        ts::as_read_only(a), ts::as_read_only(b)).sync();
    TS_CHECK(r2 == 50);
}

// BARE generic lambda (no tags): per-position modes come from the rvalue probe --
// `const auto&` = read, `auto&` = write. Same effect as the tagged and non-generic forms.
void test_multi_async_probed_generic()
{
    ts::Guarded<int> a{ 3 }, b{ 0 };
    ts::async([](const auto& x, auto& y) { y = x * 2; }, a, b).sync();
    TS_CHECK(read_value(a) == 3);
    TS_CHECK(read_value(b) == 6);

    // Mixed spellings in one generic functor probe independently per position.
    int r = ts::access([](auto& x, const auto& y) { x += y; return x; }, a, b).sync();
    TS_CHECK(r == 9);              // a += b -> 3 + 6
    TS_CHECK(read_value(a) == 9);
    TS_CHECK(read_value(b) == 6);  // read position untouched
}

// A write tag over a functor that only READS (a `const T&` parameter) is a legal conservative
// over-declaration: the object is held exclusively, and `T&` binds the `const T&` parameter.
void test_multi_async_overdeclared_write()
{
    ts::Guarded<int> a{ 5 }, b{ 0 };
    ts::async([](const int& x, int& y) { y = x; },
              ts::as_read_write(a), ts::as_read_write(b)).sync();
    TS_CHECK(read_value(b) == 5);
}

// A tagged `as_read_write` on both objects holds them exclusively (matching the non-generic
// `test_multi_async_exclusion`): a concurrent single-object async never overlaps.
void test_multi_async_generic_exclusion()
{
    ts::Guarded<Guarded> x, y;
    std::vector<ts::Task<void>> tasks;
    for (int i = 0; i < 4; ++i)
    {
        tasks.push_back(ts::async([](auto& a, auto& b) { a.touch(); b.touch(); },
                                  ts::as_read_write(x), ts::as_read_write(y)));
        tasks.push_back(x.async([](Guarded& g) { g.touch(); }));
        tasks.push_back(y.async([](Guarded& g) { g.touch(); }));
    }
    for (auto& t : tasks)
        t.sync();
    TS_CHECK(peak_of(x) == 1);   // never concurrent on x
    TS_CHECK(peak_of(y) == 1);   // never concurrent on y
}

// Object handoff: a write chain (each node writes the same object) hands the exclusive hold
// node-to-node instead of releasing + re-acquiring it. Correctness -- the value threads
// through the chain -- and re-runnability confirm the hold is preserved across the handoffs.
void test_handoff_write_chain()
{
    ts::Guarded<int> x{ 0 };
    ts::Static_task_graph g;
    ts::Graph_node a = g.add_node([](int& v) { v += 1; }, x);
    ts::Graph_node b = g.add_node([](int& v) { v *= 10; }, x);
    ts::Graph_node c = g.add_node([](int& v) { v += 5; }, x);
    b.after(a);
    c.after(b);
    g.compile();

    g.execute().sync();
    TS_CHECK(read_value(x) == 15);         // ((0 + 1) * 10) + 5 -- each handed the hold in order

    g.execute().sync();
    TS_CHECK(read_value(x) == 165);        // re-run: ((15 + 1) * 10) + 5
}

// Mixed mode across an edge does NOT hand off: a writer node then a reader node on the same
// object -- different modes, so the writer releases and the reader acquires its own hold.
// Correctness (the read sees the write) confirms the release/re-acquire path still works.
void test_handoff_skips_mode_change()
{
    ts::Guarded<int> x{ 3 };
    std::atomic<int> seen{ -1 };
    ts::Static_task_graph g;
    ts::Graph_node w = g.add_node([](int& v) { v = 42; }, x);            // write
    ts::Graph_node r = g.add_node([&seen](const int& v) { seen.store(v); }, x);   // read (mode change -> no handoff)
    r.after(w);
    g.compile();
    g.execute().sync();
    TS_CHECK(seen.load() == 42);
}

// J: repeat a concurrency-sensitive workload to catch flakiness. Each iteration uses a
// deterministic gate (two readers wait for each other) rather than a hoped "peak > 1".
void test_repeat_stress()
{
    bool all = true;
    for (int iter = 0; iter < 20; ++iter)
    {
        tests::Parallel_gate gate{ 2 };
        ts::Guarded<int> data{ 0 };
        std::vector<ts::Task<int>> tasks;

        for (int i = 0; i < 8; ++i)
            tasks.push_back(data.async([&gate](const int& v) { gate.arrive(); return v; }));
        for (auto& t : tasks)
            t.sync();

        all = all && gate.met();
    }
    TS_CHECK(all);
}

// Drive the whole mock game-engine frame (graph + Versioned transforms +
// internal parallelism + Guarded::async + then/when_all) and assert frame-level
// invariants. Reaching the assertions at all proves no deadlock and -- since
// the access harness is live -- zero access violations (a violation would have
// aborted the process).
void test_engine_frame()
{
    double avg_ms = 0.0, serial_ms = 0.0;
    float transform0 = 0.0f;
    sample::game_frame_stats(10, 1.0f, avg_ms, serial_ms, transform0);

    TS_CHECK(transform0 == 5.0f); // deterministic, correct output (2 + 3)
}

void test_engine_determinism()
{
    double avg_ms = 0.0, serial_ms = 0.0;
    float a = 0.0f, b = 0.0f;
    sample::game_frame_stats(5, 0.3f, avg_ms, serial_ms, a);
    sample::game_frame_stats(5, 0.3f, avg_ms, serial_ms, b);
    TS_CHECK(a == b);
}

// A naive blocking fork-join: launch a task per item, then get() each.
template<typename Fn>
void naive_parallel_for(int n, Fn fn)
{
    std::vector<ts::Task<void>> tasks;
    for (int i = 0; i < n; ++i)
        tasks.push_back(ts::launch([fn, i] { fn(i); }));
    for (auto& t : tasks)
        t.sync();
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
    {
        runner.detach();     // deadlocked; leak the stuck thread
    }
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
            join.sync();   // deep-retract: run a/b/c inline, then the join
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
    {
        runner.detach();
    }
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
                        .sync();   // deep-retract: run the producer inline, its callback fires the continuation
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
    {
        runner.detach();
    }
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
            int s = ts::when_all(a, b).then([](int x, int y) { return x + y; }).sync();
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
    {
        runner.detach();
    }
}

} // namespace

// Worker-less global scheduler (`Scheduler_scope{{.single_threaded = true}}`): every layer
// runs inline on the calling thread -- launch/then, Guarded async, parallel_for, and a
// graph with conflict-derived edges and a nested task. Everything must complete on the
// main thread, and a launch/async is already done when the call returns.
void test_single_threaded_end_to_end()
{
    ts::Scheduler_scope scope{ { .single_threaded = true } };
    const std::thread::id main_id = std::this_thread::get_id();

    // Bare launch + then: inline, done at return, on this thread.
    std::thread::id launch_ran_on{};
    ts::Task<int> t = ts::launch([&launch_ran_on] { launch_ran_on = std::this_thread::get_id(); return 6; });
    TS_CHECK(t.is_done());
    TS_CHECK(launch_ran_on == main_id);
    ts::Task<int> t2 = t.then([](int v) { return v * 7; });
    TS_CHECK(t2.is_done());
    TS_CHECK(t2.sync() == 42);

    // Guarded async: pipe job runs inline at admission.
    ts::Guarded<tests::Counter> c;
    c.async([](tests::Counter& k) { k.add(5); });
    TS_CHECK(c.async([](const tests::Counter& k) { return k.value(); }).sync() == 5);

    // parallel_for: all slices on the caller.
    std::array<int, 64> data{};
    ts::parallel_for(64, [&data, main_id](int i)
    {
        if (std::this_thread::get_id() == main_id)
            data[static_cast<std::size_t>(i)] = i;
    });
    int sum = 0;
    for (int v : data) sum += v;
    TS_CHECK(sum == 64 * 63 / 2);   // every slice ran, all on the main thread

    // Graph: conflict edge (writer -> reader on the same object) + a nested task in the
    // writer; the reader must observe both writes, and every body runs on the main thread.
    ts::Guarded<tests::Counter> g_obj;
    std::atomic<int> reader_saw{ -1 };
    std::atomic<bool> off_thread{ false };
    ts::Static_task_graph g;
    g.add_node([main_id, &off_thread](tests::Counter& k)
    {
        if (std::this_thread::get_id() != main_id) off_thread.store(true);
        k.add(1);
        ts::nested([&k, main_id, &off_thread]
        {
            if (std::this_thread::get_id() != main_id) off_thread.store(true);
            k.add(2);
        });
    }, g_obj);
    g.add_node([&reader_saw, main_id, &off_thread](const tests::Counter& k)
    {
        if (std::this_thread::get_id() != main_id) off_thread.store(true);
        reader_saw.store(k.value());
    }, g_obj);
    g.compile();

    ts::Task<void> done = g.execute();
    TS_CHECK(done.is_done());   // the whole run happened inside execute()
    TS_CHECK(!off_thread.load());
    TS_CHECK(reader_saw.load() == 3);   // writer body + its nested write, both before the reader
}

// The drain-before-park rule: a body that admits pipe work and then blocks on it must not
// deadlock -- `sync()` drains the thread's serial trampoline (where the admitted job sits,
// behind the running body's frame) before parking.
void test_single_threaded_sync_inside_body()
{
    ts::Scheduler_scope scope{ { .single_threaded = true } };

    ts::Guarded<tests::Counter> c;
    int seen = ts::launch([&c]
    {
        ts::Task<int> inner = c.async([](tests::Counter& k) { k.add(9); return k.value(); });
        return inner.sync();   // admitted behind this frame; the drain hook runs it
    }).sync();
    TS_CHECK(seen == 9);
}

// Two worker-less runs of a conflict-shaped graph execute the nodes in the same order:
// worker-less dispatch is deterministic (single thread, FIFO trampoline).
void test_single_threaded_deterministic_order()
{
    ts::Scheduler_scope scope{ { .single_threaded = true } };

    ts::Guarded<int> a{ 0 }, b{ 0 };
    std::vector<int> order;
    ts::Static_task_graph g;
    g.add_node([&order](int& x) { order.push_back(0); x = 1; }, a);
    g.add_node([&order](const int&) { order.push_back(1); }, a);
    g.add_node([&order](int& y) { order.push_back(2); y = 1; }, b);
    g.add_node([&order](const int&, const int&) { order.push_back(3); }, a, b);
    g.compile();

    g.execute().sync();
    std::vector<int> first = order;
    order.clear();
    g.execute().sync();
    TS_CHECK(order == first);   // bit-identical node order across runs
    TS_CHECK(order.size() == 4);
}

#if TS_SAFETY_CHECKS
// Coroutine-first §4.1: a node body that would genuinely park on a pipe job (an object the
// task does not hold) is FATAL, not a warning -- the park occupies a worker and risks
// pool-exhaustion deadlock. The sanctioned form is `co_await` (companion:
// `test_coro_await_access_in_node`, coroutine_tests).
void test_blocking_sync_in_task_is_fatal()
{
    TS_CHECK(ts::test::expect_death("sync_in_task"));
}

// Sanctioned fork-join inside a node produces zero reports: `parallel_for` joins via the
// state's own wait (never `retract_or_wait`), and bare-task joins retract.
void test_parallel_for_in_node_no_reports()
{
    long long base = ts::ensure_failure_count();
    ts::Guarded<std::array<int, 256>> data{};
    ts::Static_task_graph g;
    g.add_node([](std::array<int, 256>& d)
    {
        ts::parallel_for(256, [&d](int i) { d[static_cast<std::size_t>(i)] = i; });
    }, data);
    g.compile();
    g.execute().sync();

    TS_CHECK(ts::ensure_failure_count() == base);
    ts::Task<int> total = ts::async([](const std::array<int, 256>& d)
    {
        int s = 0;
        for (int v : d)
            s += v;
        return s;
    }, data);
    TS_CHECK(total.sync() == 256 * 255 / 2);
}
#endif

void run_integration_tests()
{
    std::printf("\n[integration] tests\n");
    run("await graph completion", test_await_graph_completion);
    run("await join into graph", test_await_join_into_graph);
    run("graph then dynamic", test_graph_then_dynamic);
    run("graph/async no overlap (during)", test_graph_async_no_overlap_during);
    run("graph/async no overlap (before)", test_async_before_graph_no_overlap);
    run("graph/async contention", test_graph_async_stress);
    run("early release frees object mid-run", test_early_release_frees_object_mid_run);
    run("lazy acquire keeps late object free", test_lazy_acquire_late_object_free_early);
    run("gap frees object between accessors", test_gap_frees_object_between_accessors);
    run("reader node overlaps async read", test_reader_node_overlaps_async_read);
    run("multi async basic", test_multi_async_basic);
    run("multi async exclusion", test_multi_async_exclusion);
    run("single-threaded: end to end", test_single_threaded_end_to_end);
    run("single-threaded: sync inside body", test_single_threaded_sync_inside_body);
    run("single-threaded: deterministic order", test_single_threaded_deterministic_order);
#if TS_SAFETY_CHECKS
    run("death: blocking sync in task", test_blocking_sync_in_task_is_fatal);
    run("parallel_for in node: no reports", test_parallel_for_in_node_no_reports);
#endif
    run("multi async no deadlock", test_multi_async_no_deadlock);
    run("multi async options", test_multi_async_options);
    run("multi async generic lambda", test_multi_async_generic_lambda);
    run("multi async probed generic", test_multi_async_probed_generic);
    run("multi async overdeclared write", test_multi_async_overdeclared_write);
    run("multi async generic exclusion", test_multi_async_generic_exclusion);
    run("handoff write chain", test_handoff_write_chain);
    run("handoff skips mode change", test_handoff_skips_mode_change);
    run("repeat stress x20", test_repeat_stress);
    run("engine frame invariants", test_engine_frame);
    run("engine determinism", test_engine_determinism);
    run("oversubscription no deadlock", test_oversubscription_no_deadlock);
    run("deep retraction no deadlock", test_deep_retraction_no_deadlock);
    run("then retraction no deadlock", test_then_retraction_no_deadlock);
    run("when_all retraction no deadlock", test_when_all_retraction_no_deadlock);
}
