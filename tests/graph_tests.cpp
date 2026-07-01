#include "graph_tests.h"
#include "thread_safe.h"
#include "static_task_graph.h"
#include "harness.h"
#include "test_util.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;
using ts::test::run;

namespace
{

int read_value(ts::Thread_safe<int>& d)
{
    return d.async([](const int& v) { return v; }).get();
}

// --- G: construction / compile + H: execution -----------------------------

void test_access_ordering()
{
    ts::Thread_safe<int> a{ 0 }, b{ 0 }, c{ 0 };

    ts::Static_task_graph g;
    g.add_node([](int& x) { x = 1; }, a);
    g.add_node([](const int& x, int& y) { y = x * 10; }, a, b);
    g.add_node([](const int& x, const int& y, int& z) { z = x + y; }, a, b, c);
    g.compile();

    g.execute().get();
    TS_CHECK(read_value(a) == 1);
    TS_CHECK(read_value(b) == 10);
    TS_CHECK(read_value(c) == 11);

    g.execute().get();   // re-run is deterministic
    TS_CHECK(read_value(c) == 11);
}

void test_explicit_ordering()
{
    ts::Thread_safe<int> p{ 0 }, q{ 0 };
    std::atomic<int> seq{ 0 };
    std::atomic<int> p_order{ 0 }, q_order{ 0 };

    ts::Static_task_graph g;
    ts::Graph_node np = g.add_node([&seq, &p_order](int&) { p_order.store(++seq); }, p);
    ts::Graph_node nq = g.add_node([&seq, &q_order](int&) { q_order.store(++seq); }, q);
    nq.after(np);
    g.compile();

    g.execute().get();
    TS_CHECK(p_order.load() == 1);
    TS_CHECK(q_order.load() == 2);
}

void test_independent_parallel()
{
    tests::Parallel_gate gate{ 2 };
    ts::Thread_safe<int> a{ 0 }, b{ 0 };

    ts::Static_task_graph g;
    auto job = [&gate](int&) { gate.arrive(); };
    g.add_node(job, a);
    g.add_node(job, b);
    g.compile();

    g.execute().get();
    TS_CHECK(gate.met());   // no conflict -> nodes run concurrently
}

void test_re_run_counts()
{
    ts::Thread_safe<int> a{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { ++v; }, a);
    g.compile();

    for (int i = 0; i < 5; ++i)
        g.execute().get();
    TS_CHECK(read_value(a) == 5);
}

void test_empty_graph()
{
    ts::Static_task_graph g;
    g.compile();
    g.execute().get();
    TS_CHECK(true);   // completes immediately
}

void test_single_node()
{
    ts::Thread_safe<int> a{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { v = 1; }, a);
    g.compile();
    g.execute().get();
    TS_CHECK(read_value(a) == 1);
}

void test_diamond()
{
    tests::Parallel_gate gate{ 2 };
    ts::Thread_safe<int> x{ 0 }, y{ 0 }, z{ 0 }, w{ 0 };

    ts::Static_task_graph g;
    g.add_node([](int& v) { v = 1; }, x);
    g.add_node([&gate](const int& xv, int& yv) { gate.arrive(); yv = xv + 1; }, x, y);
    g.add_node([&gate](const int& xv, int& zv) { gate.arrive(); zv = xv + 2; }, x, z);
    g.add_node([](const int& yv, const int& zv, int& wv) { wv = yv + zv; }, y, z, w);
    g.compile();

    g.execute().get();
    TS_CHECK(read_value(w) == 5);    // (1+1) + (1+2)
    TS_CHECK(gate.met());            // the two middle nodes ran concurrently
}

constexpr int wide = 32;

void test_completion_after_all()
{
    std::array<ts::Thread_safe<int>, wide> data{};
    std::atomic<int> ran{ 0 };

    ts::Static_task_graph g;
    for (auto& d : data)
        g.add_node([&ran](int&) { std::this_thread::sleep_for(1ms); ran.fetch_add(1); }, d);
    g.compile();

    g.execute().get();
    TS_CHECK(ran.load() == wide);   // get() returned only after every node
}

void test_graph_stress()
{
    std::array<ts::Thread_safe<int>, wide> data{};
    ts::Static_task_graph g;
    for (auto& d : data)
        g.add_node([](int& v) { ++v; }, d);
    g.compile();

    for (int run_i = 0; run_i < 10; ++run_i)
        g.execute().get();

    bool all = true;
    for (auto& d : data)
        all = all && (read_value(d) == 10);
    TS_CHECK(all);
}

void test_cancel_skips_nodes()
{
    std::array<ts::Thread_safe<int>, wide> data{};
    std::atomic<int> ran{ 0 };

    ts::Static_task_graph g;
    for (auto& d : data)
        g.add_node([&ran](int& v) { ran.fetch_add(1); v = 1; }, d);
    g.compile();

    ts::Cancellation_source src;
    src.request_cancel();               // cancel before running
    ts::Task<void> run = g.execute(ts::default_scheduler(), src.token());
    run.get();

    TS_CHECK(run.is_cancelled());
    TS_CHECK(ran.load() == 0);          // every node skipped
}

void test_death_cycle()            { TS_CHECK(ts::test::expect_death("graph_cycle")); }
void test_death_before_compile()   { TS_CHECK(ts::test::expect_death("execute_before_compile")); }
void test_death_undeclared()       { TS_CHECK(ts::test::expect_death("graph_undeclared")); }

} // namespace

void run_graph_tests()
{
    std::printf("\n[graph] tests\n");
    run("access ordering", test_access_ordering);
    run("explicit ordering", test_explicit_ordering);
    run("independent parallel", test_independent_parallel);
    run("re-run counts", test_re_run_counts);
    run("empty graph", test_empty_graph);
    run("single node", test_single_node);
    run("diamond", test_diamond);
    run("completion after all", test_completion_after_all);
    run("graph stress", test_graph_stress);
    run("cancel skips nodes", test_cancel_skips_nodes);
    run("death: cycle", test_death_cycle);
    run("death: execute before compile", test_death_before_compile);
    run("death: undeclared access", test_death_undeclared);
}
