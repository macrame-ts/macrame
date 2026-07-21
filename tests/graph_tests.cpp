#include "graph_tests.h"
#include "ts/guarded.h"
#include "ts/static_task_graph.h"
#include "harness.h"
#include "test_util.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using ts::test::run;

namespace
{

int read_value(ts::Guarded<int>& d)
{
    return d.async([](const int& v) { return v; }).sync();
}

// --- G: construction / compile + H: execution -----------------------------

void test_access_ordering()
{
    ts::Guarded<int> a{ 0 }, b{ 0 }, c{ 0 };

    ts::Static_task_graph g;
    g.add_node([](int& x) { x = 1; }, a);
    g.add_node([](const int& x, int& y) { y = x * 10; }, a, b);
    g.add_node([](const int& x, const int& y, int& z) { z = x + y; }, a, b, c);
    g.compile();

    g.execute().sync();
    TS_CHECK(read_value(a) == 1);
    TS_CHECK(read_value(b) == 10);
    TS_CHECK(read_value(c) == 11);

    g.execute().sync();   // re-run is deterministic
    TS_CHECK(read_value(c) == 11);
}

// Generic-lambda nodes: `[](auto&...)` has no introspectable parameter const-ness, so mode is
// declared with `ts::as_read_only`/`as_read_write` tags. Same graph shape as `test_access_ordering`; the
// tagged modes must derive the SAME edges (write-then-read chain), proven by the propagated
// values.
void test_generic_lambda_node()
{
    ts::Guarded<int> a{ 0 }, b{ 0 }, c{ 0 };

    ts::Static_task_graph g;
    g.add_node([](auto& x) { x = 1; }, ts::as_read_write(a));
    g.add_node([](auto& x, auto& y) { y = x * 10; }, ts::as_read_only(a), ts::as_read_write(b));
    g.add_node([](auto& x, auto& y, auto& z) { z = x + y; },
               ts::as_read_only(a), ts::as_read_only(b), ts::as_read_write(c));
    g.compile();

    g.execute().sync();
    TS_CHECK(read_value(a) == 1);
    TS_CHECK(read_value(b) == 10);
    TS_CHECK(read_value(c) == 11);   // ordering held -> tagged modes derived the same edges
}

// `ts::as_read_only` tags produce read access: two generic-lambda reader nodes on the same object
// run concurrently (a writer orders before them), matching the non-generic reader-overlap
// behavior.
void test_generic_lambda_readers_overlap()
{
    ts::Guarded<int> x{ 0 }, y{ 0 }, z{ 0 };
    tests::Parallel_gate gate{ 2 };

    ts::Static_task_graph g;
    g.add_node([](auto& v) { v = 1; }, ts::as_read_write(x));
    g.add_node([&gate](auto& xv, auto& yv) { gate.arrive(); yv = xv; }, ts::as_read_only(x), ts::as_read_write(y));
    g.add_node([&gate](auto& xv, auto& zv) { gate.arrive(); zv = xv; }, ts::as_read_only(x), ts::as_read_write(z));
    g.compile();

    g.execute().sync();
    TS_CHECK(read_value(y) == 1 && read_value(z) == 1);   // both read the writer's value
    TS_CHECK(gate.met());                                 // the two readers ran concurrently
}

// BARE generic-lambda nodes (no tags): modes come from the rvalue probe -- `const auto&` =
// read, `auto&` = write. Same shape as `test_generic_lambda_node`; the probed modes must derive
// the same write-then-read edges, proven by the propagated values.
void test_probed_generic_node()
{
    ts::Guarded<int> a{ 0 }, b{ 0 }, c{ 0 };

    ts::Static_task_graph g;
    g.add_node([](auto& x) { x = 1; }, a);
    g.add_node([](const auto& x, auto& y) { y = x * 10; }, a, b);
    g.add_node([](const auto& x, const auto& y, auto& z) { z = x + y; }, a, b, c);
    g.compile();

    g.execute().sync();
    TS_CHECK(read_value(a) == 1);
    TS_CHECK(read_value(b) == 10);
    TS_CHECK(read_value(c) == 11);   // ordering held -> probed modes derived the same edges
}

// Probed `const auto&` positions are reads: two bare-generic reader nodes on the same object
// run concurrently, matching the tagged and non-generic reader-overlap behavior.
void test_probed_generic_readers_overlap()
{
    ts::Guarded<int> x{ 0 }, y{ 0 }, z{ 0 };
    tests::Parallel_gate gate{ 2 };

    ts::Static_task_graph g;
    g.add_node([](auto& v) { v = 1; }, x);
    g.add_node([&gate](const auto& xv, auto& yv) { gate.arrive(); yv = xv; }, x, y);
    g.add_node([&gate](const auto& xv, auto& zv) { gate.arrive(); zv = xv; }, x, z);
    g.compile();

    g.execute().sync();
    TS_CHECK(read_value(y) == 1 && read_value(z) == 1);
    TS_CHECK(gate.met());   // the two probed readers ran concurrently
}

void test_explicit_ordering()
{
    ts::Guarded<int> p{ 0 }, q{ 0 };
    std::atomic<int> seq{ 0 };
    std::atomic<int> p_order{ 0 }, q_order{ 0 };

    ts::Static_task_graph g;
    ts::Graph_node np = g.add_node([&seq, &p_order](int&) { p_order.store(++seq); }, p);
    ts::Graph_node nq = g.add_node([&seq, &q_order](int&) { q_order.store(++seq); }, q);
    nq.after(np);
    g.compile();

    g.execute().sync();
    TS_CHECK(p_order.load() == 1);
    TS_CHECK(q_order.load() == 2);
}

void test_independent_parallel()
{
    tests::Parallel_gate gate{ 2 };
    ts::Guarded<int> a{ 0 }, b{ 0 };

    ts::Static_task_graph g;
    auto job = [&gate](int&) { gate.arrive(); };
    g.add_node(job, a);
    g.add_node(job, b);
    g.compile();

    g.execute().sync();
    TS_CHECK(gate.met());   // no conflict -> nodes run concurrently
}

void test_re_run_counts()
{
    ts::Guarded<int> a{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { ++v; }, a);
    g.compile();

    for (int i = 0; i < 5; ++i)
        g.execute().sync();
    TS_CHECK(read_value(a) == 5);
}

void test_empty_graph()
{
    ts::Static_task_graph g;
    g.compile();
    g.execute().sync();
    TS_CHECK(true);   // completes immediately
}

void test_single_node()
{
    ts::Guarded<int> a{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { v = 1; }, a);
    g.compile();
    g.execute().sync();
    TS_CHECK(read_value(a) == 1);
}

void test_diamond()
{
    tests::Parallel_gate gate{ 2 };
    ts::Guarded<int> x{ 0 }, y{ 0 }, z{ 0 }, w{ 0 };

    ts::Static_task_graph g;
    g.add_node([](int& v) { v = 1; }, x);
    g.add_node([&gate](const int& xv, int& yv) { gate.arrive(); yv = xv + 1; }, x, y);
    g.add_node([&gate](const int& xv, int& zv) { gate.arrive(); zv = xv + 2; }, x, z);
    g.add_node([](const int& yv, const int& zv, int& wv) { wv = yv + zv; }, y, z, w);
    g.compile();

    g.execute().sync();
    TS_CHECK(read_value(w) == 5);    // (1+1) + (1+2)
    TS_CHECK(gate.met());            // the two middle nodes ran concurrently
}

constexpr int wide = 32;

void test_completion_after_all()
{
    std::array<ts::Guarded<int>, wide> data{};
    std::atomic<int> ran{ 0 };

    ts::Static_task_graph g;
    for (auto& d : data)
        g.add_node([&ran](int&) { std::this_thread::sleep_for(1ms); ran.fetch_add(1); }, d);
    g.compile();

    g.execute().sync();
    TS_CHECK(ran.load() == wide);   // get() returned only after every node
}

void test_graph_stress()
{
    std::array<ts::Guarded<int>, wide> data{};
    ts::Static_task_graph g;
    for (auto& d : data)
        g.add_node([](int& v) { ++v; }, d);
    g.compile();

    for (int run_i = 0; run_i < 10; ++run_i)
        g.execute().sync();

    bool all = true;
    for (auto& d : data)
        all = all && (read_value(d) == 10);
    TS_CHECK(all);
}

void test_cancel_skips_nodes()
{
    std::array<ts::Guarded<int>, wide> data{};
    std::atomic<int> ran{ 0 };

    ts::Static_task_graph g;
    for (auto& d : data)
        g.add_node([&ran](int& v) { ran.fetch_add(1); v = 1; }, d);
    g.compile();

    ts::Cancellation_source src;
    src.request_cancel();               // cancel before running
    ts::Task<void> run = g.execute(ts::default_scheduler(), src.token());
    run.sync();

    TS_CHECK(run.is_cancelled());
    TS_CHECK(ran.load() == 0);          // every node skipped
}

// A node fans out nested tasks; the run must not complete until every one settles.
void test_nested_gates_completion()
{
    constexpr int n = 8;
    ts::Guarded<int> owned{ 0 };
    std::atomic<int> done_count{ 0 };

    ts::Static_task_graph g;
    g.add_node([&done_count](int&)
    {
        for (int k = 0; k < n; ++k)
            ts::nested([&done_count] { done_count.fetch_add(1, std::memory_order_relaxed); });
    }, owned);
    g.compile();

    g.execute().sync();
    TS_CHECK(done_count.load() == n);   // get() returned only after every nested task
}

// A nested task touches the node's OWNED guarded object; it runs under the node's
// inherited access grant, so the harness must accept it.
void test_nested_inherits_access()
{
    ts::Guarded<tests::Counter> c;

    ts::Static_task_graph g;
    g.add_node([](tests::Counter& counter)
    {
        ts::nested([&counter] { counter.add(5); });   // guarded write, on a worker
    }, c);
    g.compile();

    g.execute().sync();
    int v = c.async([](const tests::Counter& k) { return k.value(); }).sync();
    TS_CHECK(v == 5);
}

// The builder path (ts::task().launch() + add_nested) must inherit the node's grant too
// -- not just ts::nested. Guards against the launch/task access-inheritance asymmetry.
void test_builder_nested_inherits_access()
{
    ts::Guarded<tests::Counter> c;

    ts::Static_task_graph g;
    g.add_node([](tests::Counter& counter)
    {
        ts::Task<void> t = ts::task([&counter] { counter.add(7); }).launch();   // builder, not ts::nested
        ts::add_nested(t);
    }, c);
    g.compile();

    g.execute().sync();
    int v = c.async([](const tests::Counter& k) { return k.value(); }).sync();
    TS_CHECK(v == 7);
}

// Nested sub-work gates a conflicting successor: the reader node must see every write
// the writer node's nested tasks made (they gate its completion, which orders the edge).
void test_nested_before_successor()
{
    constexpr int n = 16;
    ts::Guarded<std::array<int, n>> arr{};
    std::atomic<int> sum_seen{ -1 };

    ts::Static_task_graph g;
    g.add_node([](std::array<int, n>& a)
    {
        for (int k = 0; k < n; ++k)
            ts::nested([&a, k] { a[k] = k + 1; });   // disjoint elements: no race
    }, arr);
    g.add_node([&sum_seen](const std::array<int, n>& a)
    {
        int s = 0;
        for (int v : a) s += v;
        sum_seen.store(s);
    }, arr);
    g.compile();

    g.execute().sync();
    TS_CHECK(sum_seen.load() == n * (n + 1) / 2);   // reader ran after every nested write
}

// Per-node priority: a writer root gates three reader successors, released together when
// it completes; on a single worker they run in priority order. Deterministic because the
// root queues all three (in its node_complete) before the worker picks the next task.
void test_node_priority_order()
{
    ts::Scheduler s{ { .num_threads = 1 } };
    ts::Guarded<int> a{ 0 };
    std::atomic<int> seq{ 0 };
    std::atomic<int> high_ord{ 0 }, normal_ord{ 0 }, low_ord{ 0 };

    ts::Static_task_graph g;
    g.add_node([](int& v) { v = 1; }, a);   // writer root -> the three readers run after it
    g.add_node([&seq, &low_ord](const int&) { low_ord.store(seq.fetch_add(1)); }, a).priority(ts::Priority::low);
    g.add_node([&seq, &normal_ord](const int&) { normal_ord.store(seq.fetch_add(1)); }, a).priority(ts::Priority::normal);
    g.add_node([&seq, &high_ord](const int&) { high_ord.store(seq.fetch_add(1)); }, a).priority(ts::Priority::high);
    g.compile();

    g.execute(s).sync();
    TS_CHECK(high_ord.load() < normal_ord.load());    // high ran before normal
    TS_CHECK(normal_ord.load() < low_ord.load());     // normal ran before low
}

// An inline root node runs on the execute() caller thread (it settled + acquired its own
// object synchronously). Deterministic: the object is free at kickoff, so the acquire
// succeeds synchronously and the node dispatches inline on this thread.
void test_graph_node_inline_on_caller()
{
    ts::Guarded<int> x{ 0 };
    std::atomic<std::thread::id> node_thread{};

    ts::Static_task_graph g;
    g.add_node([&node_thread](int& v) { node_thread.store(std::this_thread::get_id()); v = 1; }, x).set_inline();
    g.compile();
    g.execute().sync();

    TS_CHECK(node_thread.load() == std::this_thread::get_id());   // inline root ran on the caller
    TS_CHECK(read_value(x) == 1);
}

// A chain of inline nodes runs entirely on the settling thread (here the caller): the root
// runs inline, releases its object, its inline successor acquires it synchronously and runs
// inline too (trampolined). Order preserved by the edge.
void test_graph_inline_chain_on_caller()
{
    ts::Guarded<int> x{ 0 };
    std::atomic<int> seq{ 0 };
    std::atomic<int> a_ord{ 0 }, b_ord{ 0 };
    std::atomic<std::thread::id> a_thr{}, b_thr{};

    ts::Static_task_graph g;
    ts::Graph_node a = g.add_node([&](int& v) { a_ord.store(++seq); a_thr.store(std::this_thread::get_id()); v = 1; }, x).set_inline();
    ts::Graph_node b = g.add_node([&](int& v) { b_ord.store(++seq); b_thr.store(std::this_thread::get_id()); v = 2; }, x).set_inline();
    b.after(a);
    g.compile();
    g.execute().sync();

    TS_CHECK(a_ord.load() == 1 && b_ord.load() == 2);            // order preserved
    TS_CHECK(a_thr.load() == std::this_thread::get_id());
    TS_CHECK(b_thr.load() == std::this_thread::get_id());        // whole inline chain on the caller
    TS_CHECK(read_value(x) == 2);
}

// Inline nodes are re-runnable like any node (the block re-arms; run_inline sticks).
void test_graph_inline_rerun()
{
    ts::Guarded<int> x{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { ++v; }, x).set_inline();
    g.compile();
    for (int i = 0; i < 5; ++i)
        g.execute().sync();
    TS_CHECK(read_value(x) == 5);
}

// The DOT structure dump (`compile(dot_path)`): named / unnamed labels, derived edges
// dashed with a conflict tooltip, an explicit edge solid (its coinciding conflict kept
// as the tooltip).
void test_dot_dump()
{
    ts::Guarded<int> x{ ts::Named{"counter"}, 0 };   // named -> tooltip uses the name
    ts::Guarded<int> y{ 0 };                         // unnamed -> ordinal fallback

    ts::Static_task_graph g;
    g.add_node("writer_a", [](int& v) { v = 1; }, x);
    ts::Graph_node b = g.add_node("reader_b", [](const int& v, int& w) { w = v; }, x, y);
    ts::Graph_node c = g.add_node([](const int& w) { (void)w; }, y);   // unnamed -> "node2"
    c.after(b);   // explicit, on top of the derived y-conflict (dedups to one solid edge)

    const char* path = "graph_dump_test.dot";
    g.compile(path);

    std::string dot;
    {
        std::ifstream f(path, std::ios::binary);
        dot.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::remove(path);

    TS_CHECK(dot.rfind("digraph\n", 0) == 0);   // anonymous: a graph name would become a hover tooltip on every miss
    TS_CHECK(dot.find("subgraph cluster_legend") != std::string::npos);
    TS_CHECK(dot.find("writer_a") != std::string::npos);
    TS_CHECK(dot.find("reader_b") != std::string::npos);
    TS_CHECK(dot.find("node2") != std::string::npos);
    // a->b: derived from the x conflict (W->R), dashed + tooltip with x's `ts::Named` name
    TS_CHECK(dot.find("n0 -> n1 [style=dashed, color=\"#a6e22e\", penwidth=1.8, tooltip=\"counter: W->R\"") != std::string::npos);
    // b->c: explicit (solid, no dash), tooltip still carries the y conflict
    TS_CHECK(dot.find("n1 -> n2 [color=\"#f92672\", penwidth=2.6, tooltip=\"explicit ordering; obj1: W->R\"") != std::string::npos);

    // The graph still runs after a dumping compile.
    g.execute().sync();
    TS_CHECK(read_value(x) == 1);
    TS_CHECK(read_value(y) == 1);
}

void test_death_cycle()            { TS_CHECK(ts::test::expect_death("graph_cycle")); }
void test_death_before_compile()   { TS_CHECK(ts::test::expect_death("execute_before_compile")); }
void test_death_undeclared()       { TS_CHECK(ts::test::expect_death("graph_undeclared")); }

} // namespace

void run_graph_tests()
{
    std::printf("\n[graph] tests\n");
    run("access ordering", test_access_ordering);
    run("generic-lambda node", test_generic_lambda_node);
    run("generic-lambda readers overlap", test_generic_lambda_readers_overlap);
    run("probed generic node", test_probed_generic_node);
    run("probed generic readers overlap", test_probed_generic_readers_overlap);
    run("explicit ordering", test_explicit_ordering);
    run("independent parallel", test_independent_parallel);
    run("re-run counts", test_re_run_counts);
    run("empty graph", test_empty_graph);
    run("single node", test_single_node);
    run("diamond", test_diamond);
    run("completion after all", test_completion_after_all);
    run("graph stress", test_graph_stress);
    run("cancel skips nodes", test_cancel_skips_nodes);
    run("nested gates completion", test_nested_gates_completion);
    run("nested inherits access", test_nested_inherits_access);
    run("builder nested inherits access", test_builder_nested_inherits_access);
    run("nested before successor", test_nested_before_successor);
    run("node priority order", test_node_priority_order);
    run("graph node inline on caller", test_graph_node_inline_on_caller);
    run("graph inline chain on caller", test_graph_inline_chain_on_caller);
    run("graph inline rerun", test_graph_inline_rerun);
    run("dot dump", test_dot_dump);
    run("death: cycle", test_death_cycle);
    run("death: execute before compile", test_death_before_compile);
    run("death: undeclared access", test_death_undeclared);
}
