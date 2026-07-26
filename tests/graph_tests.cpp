#include "graph_tests.h"
#include "ts/guarded.h"
#include "ts/parallel_for.h"
#include "ts/static_task_graph.h"
#include "graph_trace.h"
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
    ts::Task<void> run = g.execute({ .token = src.token() });
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
    ts::Scheduler_scope s{ { .num_threads = 1 } };
    ts::Guarded<int> a{ 0 };
    std::atomic<int> seq{ 0 };
    std::atomic<int> high_ord{ 0 }, normal_ord{ 0 }, low_ord{ 0 };

    ts::Static_task_graph g;
    g.add_node([](int& v) { v = 1; }, a);   // writer root -> the three readers run after it
    g.add_node([&seq, &low_ord](const int&) { low_ord.store(seq.fetch_add(1)); }, a).priority(ts::Priority::low);
    g.add_node([&seq, &normal_ord](const int&) { normal_ord.store(seq.fetch_add(1)); }, a).priority(ts::Priority::normal);
    g.add_node([&seq, &high_ord](const int&) { high_ord.store(seq.fetch_add(1)); }, a).priority(ts::Priority::high);
    g.compile();

    g.execute().sync();
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

// The DOT structure dump (`compile(DOT_path)`): named / unnamed labels, derived edges
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
    // a->b: derived from the x conflict (writer -> reader), dashed + tooltip with x's `ts::Named` name
    TS_CHECK(dot.find("n0 -> n1 [style=dashed, color=\"#a6e22e\", penwidth=1.8, tooltip=\"counter: RW->RO\"") != std::string::npos);
    // b->c: explicit (solid, no dash), tooltip still carries the y conflict
    TS_CHECK(dot.find("n1 -> n2 [color=\"#a6e22e\", penwidth=2.0, tooltip=\"explicit ordering; obj1: RW->RO\"") != std::string::npos);

    // The graph still runs after a dumping compile.
    g.execute().sync();
    TS_CHECK(read_value(x) == 1);
    TS_CHECK(read_value(y) == 1);
}

// The aggregated runtime trace (`set_trace` + `Graph_trace`): run counts, streamed stat
// sanity, and the average-run SVG containing labels, an access line, and the global runs
// stat. Exact-string checks kept minimal (styling changes shouldn't break this test).
void test_graph_trace()
{
    ts::Guarded<int> x{ ts::Named{"counter"}, 0 };
    ts::Guarded<int> y{ 0 };

    ts::Static_task_graph g;
    g.add_node("writer_a", [](int& v) { ++v; }, x);
    g.add_node("reader_b", [](const int& v, int& w) { w = v; }, x, y);
    g.compile();

    ts::tools::Graph_trace trace;
    g.set_trace(&trace);

    constexpr int n = 20;
    for (int i = 0; i < n; ++i)
        g.execute().sync();

    TS_CHECK(trace.run_count() == n);
    for (int i = 0; i < g.node_count(); ++i)
    {
        auto s = trace.node_stats(i);
        TS_CHECK(s.runs == n);
        TS_CHECK(s.min_us <= s.P50_us && s.P50_us <= s.max_us);
        TS_CHECK(s.mean_us >= s.min_us && s.mean_us <= s.max_us);
        TS_CHECK(s.P95_us <= s.max_us);
    }

    const char* path = "graph_trace_test.svg";
    TS_CHECK(trace.write_SVG(path));
    std::string svg;
    {
        std::ifstream f(path, std::ios::binary);
        svg.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::remove(path);

    TS_CHECK(svg.find("writer_a") != std::string::npos);
    TS_CHECK(svg.find("reader_b") != std::string::npos);
    TS_CHECK(svg.find("counter: `ff5f45RW`") != std::string::npos);   // declared access (RW, red segment) in the tooltip data
    TS_CHECK(svg.find("runs: 20") != std::string::npos);     // global stats panel
    TS_CHECK(svg.find("data-tip=") != std::string::npos);    // overlay tooltip data present
    TS_CHECK(svg.find("<script>") != std::string::npos);     // the overlay script itself
    TS_CHECK(svg.find("&#10;runs") == std::string::npos);    // per-node runs line removed (global only)

    g.set_trace(nullptr);
    g.execute().sync();
    TS_CHECK(trace.run_count() == n);   // detached: no further folds
}

// A cancelled run is not folded (its stamps are partial).
void test_graph_trace_cancelled()
{
    ts::Guarded<int> x{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { ++v; }, x);
    g.compile();

    ts::tools::Graph_trace trace;
    g.set_trace(&trace);
    g.execute().sync();
    TS_CHECK(trace.run_count() == 1);

    ts::Cancellation_source src;
    src.request_cancel();
    g.execute({ .token = src.token() }).sync();   // cancelled run
    TS_CHECK(trace.run_count() == 1);                          // not folded
    g.set_trace(nullptr);
}

// The measured critical path: on a linear chain (three writers of one object, ordered by
// the derived edges) every run's binding chain is the whole chain, so criticality is 100%
// for all three nodes; dispatch-wait stats are finite and non-negative; the SVG carries
// the criticality tooltip line and the legend row.
void test_graph_trace_critical()
{
    ts::Guarded<int> x{ 0 };

    ts::Static_task_graph g;
    g.add_node("chain_a", [](int& v) { ++v; }, x);
    g.add_node("chain_b", [](int& v) { std::this_thread::sleep_for(2ms); ++v; }, x);
    g.add_node("chain_c", [](int& v) { ++v; }, x);
    g.compile();

    ts::tools::Graph_trace trace;
    g.set_trace(&trace);

    constexpr int n = 10;
    for (int i = 0; i < n; ++i)
        g.execute().sync();

    TS_CHECK(trace.run_count() == n);
    for (int i = 0; i < g.node_count(); ++i)
    {
        auto s = trace.node_stats(i);
        TS_CHECK(s.critical_share == 1.0);   // the whole linear chain binds every run
        TS_CHECK(s.dispatch_wait_us >= 0.0 && s.dispatch_wait_us < 1e9);
    }

    const char* path = "graph_trace_critical_test.svg";
    TS_CHECK(trace.write_SVG(path));
    std::string svg;
    {
        std::ifstream f(path, std::ios::binary);
        svg.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::remove(path);

    TS_CHECK(svg.find("Critical: in ") != std::string::npos);         // node tooltip data
    TS_CHECK(svg.find("critical-path edge (pink") != std::string::npos);    // legend row
    TS_CHECK(svg.find("critical node (orange") != std::string::npos);       // legend row
    g.set_trace(nullptr);
}

// Node priority reaches the trace and renders: the tooltip carries a priority line, the
// name of a high-priority node is filled with the priority red, and the legend explains
// the name-colour scheme.
void test_graph_trace_priority()
{
    ts::Guarded<int> x{ 0 }, y{ 0 };

    ts::Static_task_graph g;
    g.add_node("hi", [](int& v) { ++v; }, x).priority(ts::Priority::high);
    g.add_node("lo", [](int& v) { ++v; }, y).priority(ts::Priority::low);
    g.compile();

    ts::tools::Graph_trace trace;
    g.set_trace(&trace);
    g.execute().sync();

    const char* path = "graph_trace_priority_test.svg";
    TS_CHECK(trace.write_SVG(path));
    std::string svg;
    {
        std::ifstream f(path, std::ios::binary);
        svg.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::remove(path);

    TS_CHECK(svg.find("data-prio=\"high-pri\"") != std::string::npos);   // priority tag on the tooltip name line
    TS_CHECK(svg.find("data-prio=\"low-pri\"") != std::string::npos);
    // The name text is filled with the priority colour; the label renders inside the bar
    // (with a text-anchor) or outside (without), depending on measured widths -- accept
    // either form. Low grey is the brightened #bab8ad.
    bool hi_red = svg.find("fill=\"#ff5f45\">hi</text>") != std::string::npos
        || svg.find("fill=\"#ff5f45\" text-anchor=\"middle\">hi</text>") != std::string::npos;
    bool lo_grey = svg.find("fill=\"#bab8ad\">lo</text>") != std::string::npos
        || svg.find("fill=\"#bab8ad\" text-anchor=\"middle\">lo</text>") != std::string::npos;
    TS_CHECK(hi_red);
    TS_CHECK(lo_grey);
    TS_CHECK(svg.find("= node name colour = priority") != std::string::npos);   // legend row
    g.set_trace(nullptr);
}

// Handoff weld + dead-time + utilization rendering, on SYNTHETIC folds: `on_run_complete`
// is the trace's public fold entry, so the test drives it with fabricated tick arrays --
// the weld geometry (back-to-back on one lane) and the utilization arithmetic become
// deterministic instead of riding real inline-dispatch timing, which flaked three times
// at ever-larger budgets (the median gap's Debug jitter tracks ambient machine state).
// The real stamp/fold plumbing is covered end-to-end by test_graph_trace_end_to_end.
void test_graph_trace_weld_dead_time()
{
    auto ticks = [](double us)
    {
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::micro>(us)).count();
    };

    ts::tools::Graph_trace trace;
    trace.begin_structure(2);
    trace.set_node_label(0, "weld_a");
    trace.set_node_label(1, "weld_b");
    trace.add_node_access(0, "x", true);
    trace.add_node_access(1, "x", true);
    trace.add_edge(0, 1, false, "x: RW->RW");

    // a: [100, 1000) us on worker 0; b back to back at [1001, 1300) on the same worker
    // (1 us gap << the ~5px weld threshold); window [0, 1400) with 770 us of busy time
    // on the one worker -> utilization exactly 0.55.
    long long readys[2] = { ticks(100), ticks(1000) };
    long long starts[2] = { ticks(100), ticks(1001) };
    long long ends[2] = { ticks(1000), ticks(1300) };
    int workers[2] = { 0, 0 };
    for (int i = 0; i < 8; ++i)
        trace.on_run_complete(readys, starts, ends, workers, 2, 0, ticks(1400), ticks(770), 1);
    TS_CHECK(trace.run_count() == 8);

    const char* path = "graph_trace_weld_test.svg";
    TS_CHECK(trace.write_SVG(path));
    std::string svg;
    {
        std::ifstream f(path, std::ios::binary);
        svg.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::remove(path);

    TS_CHECK(svg.find("r=\"4.5\"") != std::string::npos);                  // the weld dot
    TS_CHECK(svg.find("Handoff: back-to-back") != std::string::npos);      // its tooltip line
    TS_CHECK(svg.find("handoff weld") != std::string::npos);               // legend row
    TS_CHECK(svg.find("critical path dead time:") != std::string::npos);   // the headline line
    TS_CHECK(svg.find("core utilization:") != std::string::npos);
    TS_CHECK(std::abs(trace.core_utilization() - 770.0 / 1400.0) < 1e-9);  // exact arithmetic
    TS_CHECK(ts::tools::dead_time_ok_share < ts::tools::dead_time_bad_share);   // band order
    TS_CHECK(ts::tools::core_util_ok_share < ts::tools::core_util_good_share);  // band order
    TS_CHECK(svg.find(">W0<") == std::string::npos);   // rows are anonymous -- no worker labels
}

// Interval packing: two time-overlapping nodes land on different rows regardless of
// worker assignment (rows are concurrency slots, not lanes). Bar rects are the rx="3"
// ones; their y attributes must differ.
void test_graph_trace_row_packing()
{
    auto ticks = [](double us)
    {
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::micro>(us)).count();
    };

    ts::tools::Graph_trace trace;
    trace.begin_structure(2);
    trace.set_node_label(0, "pack_a");
    trace.set_node_label(1, "pack_b");

    long long readys[2] = { ticks(0), ticks(100) };
    long long starts[2] = { ticks(0), ticks(100) };
    long long ends[2] = { ticks(500), ticks(600) };
    int workers[2] = { 0, 0 };   // same worker; overlap in time must still split rows
    trace.on_run_complete(readys, starts, ends, workers, 2, 0, ticks(700), ticks(700), 1);

    const char* path = "graph_trace_pack_test.svg";
    TS_CHECK(trace.write_SVG(path));
    std::string svg;
    {
        std::ifstream f(path, std::ios::binary);
        svg.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::remove(path);

    // Collect the y= attribute of each bar rect (rx="3" marks node bars).
    std::vector<std::string> bar_y;
    for (size_t pos = svg.find("rx=\"3\""); pos != std::string::npos; pos = svg.find("rx=\"3\"", pos + 1))
    {
        size_t rect = svg.rfind("<rect", pos);
        size_t y = svg.find("y=\"", rect);
        bar_y.push_back(svg.substr(y + 3, svg.find('"', y + 3) - y - 3));
    }
    TS_CHECK(bar_y.size() == 2);
    TS_CHECK(bar_y.size() == 2 && bar_y[0] != bar_y[1]);   // two rows
}

// End-to-end plumbing for the busy counters: a real graph on a one-worker scheduler must
// fold a nonzero utilization (the loose floor guards the arm/stamp/fold chain -- exact
// figures are the synthetic test's job). Includes the in-flight case: the fold runs
// INSIDE the settling worker's task, so only in-flight-aware busy accounting sees the
// window's work at all.
void test_graph_trace_end_to_end_utilization()
{
    ts::Guarded<int> x{ 0 };
    auto busy = [](int us)
    {
        auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
        while (std::chrono::steady_clock::now() < until) {}
    };

    ts::Static_task_graph g;
    g.add_node("util_a", [busy](int& v) { busy(400); ++v; }, x);
    g.add_node("util_b", [busy](int& v) { busy(200); ++v; }, x);
    g.compile();

    ts::tools::Graph_trace trace;
    g.set_trace(&trace);
    ts::Scheduler_scope one{ { .num_threads = 1 } };
    for (int i = 0; i < 8; ++i)
        g.execute().sync();
    g.set_trace(nullptr);

    TS_CHECK(trace.run_count() == 8);
    TS_CHECK(trace.core_utilization() > 0.3);
    TS_CHECK(trace.core_utilization() <= 1.0);
}

// Task volume: a node running a `parallel_for` fans out into slice tasks, so the trace's
// total task count exceeds the run count (which would equal it if each run were one task).
void test_graph_trace_task_count()
{
    ts::Guarded<int> x{ 0 };
    auto busy = [](int us)
    {
        auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
        while (std::chrono::steady_clock::now() < until) {}
    };

    ts::Static_task_graph g;
    g.add_node("fanout", [busy](int& v)
    {
        ts::parallel_for(64, [&busy](int) { busy(30); });   // fans out onto the pool
        ++v;
    }, x);
    g.compile();

    ts::tools::Graph_trace trace;
    g.set_trace(&trace);
    ts::Scheduler_scope pool{ { .num_threads = 4 } };
    constexpr int N = 6;
    for (int i = 0; i < N; ++i)
        g.execute().sync();
    g.set_trace(nullptr);

    TS_CHECK(trace.run_count() == N);
    TS_CHECK(trace.task_total() > trace.run_count());   // fan-out: slices are separate tasks
    TS_CHECK(trace.tasks_per_run() > 1.0);
}

// Task-system overhead metric on SYNTHETIC folds: `on_run_complete`'s trailing body/machinery
// deltas drive `overhead()` = M / (B + M) and the headline. Deterministic arithmetic.
void test_graph_trace_overhead()
{
    auto ticks = [](double us)
    {
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::micro>(us)).count();
    };

    ts::tools::Graph_trace trace;
    trace.begin_structure(1);
    trace.set_node_label(0, "solo");

    long long readys[1] = { ticks(0) };
    long long starts[1] = { ticks(0) };
    long long ends[1] = { ticks(900) };
    int workers[1] = { 0 };
    // body 900 us, machinery 100 us per run -> overhead exactly 0.10.
    for (int i = 0; i < 8; ++i)
        trace.on_run_complete(readys, starts, ends, workers, 1, 0, ticks(1000), ticks(900), 1,
            nullptr, 0, 0, nullptr, 1, ticks(900), ticks(100));

    TS_CHECK(std::abs(trace.overhead() - 0.10) < 1e-9);
    TS_CHECK(std::abs(trace.body_us() - 900.0) < 1e-6);
    TS_CHECK(std::abs(trace.machinery_us() - 100.0) < 1e-6);
    TS_CHECK(ts::tools::overhead_ok_share < ts::tools::overhead_bad_share);   // band order

    const char* path = "graph_trace_overhead_test.svg";
    TS_CHECK(trace.write_SVG(path));
    std::string svg;
    {
        std::ifstream f(path, std::ios::binary);
        svg.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::remove(path);
    TS_CHECK(svg.find("task-system overhead:") != std::string::npos);   // the headline figure
}

// End-to-end: a real graph on a real pool must fold nonzero body AND machinery, with a
// sane overhead share (bodies dominate at this granularity, so it stays well below 1).
void test_graph_trace_overhead_end_to_end()
{
    ts::Guarded<int> x{ 0 };
    auto busy = [](int us)
    {
        auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
        while (std::chrono::steady_clock::now() < until) {}
    };

    ts::Static_task_graph g;
    // Fan out: the in-body `parallel_for` submits slice tasks, which reclassify to machinery
    // (submit-from-within-a-functor), so machinery is deterministically nonzero -- a single
    // coarse body would leave setup below the ~100 ns clock tick and read ~0.
    g.add_node("ov_a", [busy](int& v) { ts::parallel_for(32, [&busy](int) { busy(20); }); ++v; }, x);
    g.add_node("ov_b", [busy](int& v) { ts::parallel_for(32, [&busy](int) { busy(20); }); ++v; }, x);
    g.compile();

    ts::tools::Graph_trace trace;
    g.set_trace(&trace);
    ts::Scheduler_scope pool{ { .num_threads = 4 } };
    for (int i = 0; i < 8; ++i)
        g.execute().sync();
    g.set_trace(nullptr);

    TS_CHECK(trace.run_count() == 8);
    TS_CHECK(trace.body_us() > 0.0);         // user compute measured
    TS_CHECK(trace.machinery_us() > 0.0);    // scheduler cost measured (fan-out submits + dispatch)
    TS_CHECK(trace.overhead() > 0.0);
    TS_CHECK(trace.overhead() < 0.5);        // ~1.3 ms of body dwarfs the fan-out machinery
}

void test_death_cycle()            { TS_CHECK(ts::test::expect_death("graph_cycle")); }
void test_death_before_compile()   { TS_CHECK(ts::test::expect_death("execute_before_compile")); }
void test_death_undeclared()       { TS_CHECK(ts::test::expect_death("graph_undeclared")); }
void test_death_guarded_outlived() { TS_CHECK(ts::test::expect_death("guarded_outlived_by_graph")); }
void test_death_graph_mid_run()    { TS_CHECK(ts::test::expect_death("graph_destroyed_mid_run")); }
#if TS_SAFETY_CHECKS
// The sharp same-object diagnostic (a node syncing an access to an object it holds --
// the certain-deadlock shape); subprocess because the child genuinely deadlocks after
// reporting (it aborts once the report is observed).
void test_death_sync_own_object()  { TS_CHECK(ts::test::expect_death("sync_own_object_deadlock")); }
#endif

// The pipe-registration counts stay balanced across recompiles, moves, and a move-assign
// overwrite -- so destroying the graphs and then the objects raises no lifetime fatal, and
// every configuration still runs correctly.
void test_lifetime_registration_balance()
{
    ts::Guarded<int> a{ 0 };
    ts::Guarded<int> b{ 0 };
    {
        ts::Static_task_graph g;
        g.add_node([](int& x) { x += 1; }, a);
        g.compile();
        g.compile();   // recompile releases + re-registers (balanced)
        g.execute().sync();

        ts::Static_task_graph g2 = std::move(g);   // registration rides the move
        g2.execute().sync();

        ts::Static_task_graph g3;
        g3.add_node([](int& y) { y += 10; }, b);
        g3.compile();
        g3.execute().sync();
        g3 = std::move(g2);   // overwrite releases g3's registration of `b`
        g3.execute().sync();
    }   // all graphs destroyed -> all registrations released

    TS_CHECK(a.async([](const int& x) { return x; }).sync() == 3);   // three runs of the writer
    TS_CHECK(b.async([](const int& y) { return y; }).sync() == 10);
    // `a`/`b` destruct at scope end without a lifetime fatal = the balance held.
}

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
    run("graph trace", test_graph_trace);
    run("graph trace cancelled run", test_graph_trace_cancelled);
    run("graph trace critical path", test_graph_trace_critical);
    run("graph trace priority", test_graph_trace_priority);
    run("graph trace weld + dead time", test_graph_trace_weld_dead_time);
    run("graph trace row packing", test_graph_trace_row_packing);
    run("graph trace end-to-end utilization", test_graph_trace_end_to_end_utilization);
    run("graph trace task count", test_graph_trace_task_count);
    run("graph trace overhead", test_graph_trace_overhead);
    run("graph trace overhead end-to-end", test_graph_trace_overhead_end_to_end);
    run("death: cycle", test_death_cycle);
    run("death: execute before compile", test_death_before_compile);
    run("death: undeclared access", test_death_undeclared);
    run("death: Guarded outlived by graph", test_death_guarded_outlived);
    run("death: graph destroyed mid-run", test_death_graph_mid_run);
#if TS_SAFETY_CHECKS
    run("death: sync own object (sharp diagnostic)", test_death_sync_own_object);
#endif
    run("lifetime registration balance", test_lifetime_registration_balance);
}
