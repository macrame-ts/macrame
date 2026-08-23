#include "graph_tests.h"
#include "ts/coroutine_support.h"
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
using namespace ts::test;

namespace
{

int read_value(ts::Guarded<int>& d)
{
    return d.async([](const int& v) { return v; }).sync();
}

// --- G: construction / compile + H: execution -----------------------------

void test_access_ordering()
{
    ts::Guarded<int> a{ ts::Named{}, 0 }, b{ ts::Named{}, 0 }, c{ ts::Named{}, 0 };

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](int& x) { x = 1; }, a);
    g.add_node(ts::Named{}, [](const int& x, int& y) { y = x * 10; }, a, b);
    g.add_node(ts::Named{}, [](const int& x, const int& y, int& z) { z = x + y; }, a, b, c);
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
// tagged modes must derive the same edges (write-then-read chain), proven by the propagated
// values.
void test_generic_lambda_node()
{
    ts::Guarded<int> a{ ts::Named{}, 0 }, b{ ts::Named{}, 0 }, c{ ts::Named{}, 0 };

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](auto& x) { x = 1; }, ts::as_read_write(a));
    g.add_node(ts::Named{}, [](auto& x, auto& y) { y = x * 10; }, ts::as_read_only(a), ts::as_read_write(b));
    g.add_node(ts::Named{}, [](auto& x, auto& y, auto& z) { z = x + y; },
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
    ts::Guarded<int> x{ ts::Named{}, 0 }, y{ ts::Named{}, 0 }, z{ ts::Named{}, 0 };
    tests::Parallel_gate gate{ 2 };

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](auto& v) { v = 1; }, ts::as_read_write(x));
    g.add_node(ts::Named{}, [&gate](auto& xv, auto& yv) { gate.arrive(); yv = xv; }, ts::as_read_only(x), ts::as_read_write(y));
    g.add_node(ts::Named{}, [&gate](auto& xv, auto& zv) { gate.arrive(); zv = xv; }, ts::as_read_only(x), ts::as_read_write(z));
    g.compile();

    g.execute().sync();
    TS_CHECK(read_value(y) == 1 && read_value(z) == 1);   // both read the writer's value
    TS_CHECK(gate.met());                                 // the two readers ran concurrently
}

// bare generic-lambda nodes (no tags): modes come from the rvalue probe - `const auto&` =
// read, `auto&` = write. Same shape as `test_generic_lambda_node`; the probed modes must derive
// the same write-then-read edges, proven by the propagated values.
void test_probed_generic_node()
{
    ts::Guarded<int> a{ ts::Named{}, 0 }, b{ ts::Named{}, 0 }, c{ ts::Named{}, 0 };

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](auto& x) { x = 1; }, a);
    g.add_node(ts::Named{}, [](const auto& x, auto& y) { y = x * 10; }, a, b);
    g.add_node(ts::Named{}, [](const auto& x, const auto& y, auto& z) { z = x + y; }, a, b, c);
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
    ts::Guarded<int> x{ ts::Named{}, 0 }, y{ ts::Named{}, 0 }, z{ ts::Named{}, 0 };
    tests::Parallel_gate gate{ 2 };

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](auto& v) { v = 1; }, x);
    g.add_node(ts::Named{}, [&gate](const auto& xv, auto& yv) { gate.arrive(); yv = xv; }, x, y);
    g.add_node(ts::Named{}, [&gate](const auto& xv, auto& zv) { gate.arrive(); zv = xv; }, x, z);
    g.compile();

    g.execute().sync();
    TS_CHECK(read_value(y) == 1 && read_value(z) == 1);
    TS_CHECK(gate.met());   // the two probed readers ran concurrently
}

void test_explicit_ordering()
{
    ts::Guarded<int> p{ ts::Named{}, 0 }, q{ ts::Named{}, 0 };
    std::atomic<int> seq{ 0 };
    std::atomic<int> p_order{ 0 }, q_order{ 0 };

    ts::Static_task_graph g;
    ts::Graph_node np = g.add_node(ts::Named{}, [&seq, &p_order](int&) { p_order.store(++seq); }, p);
    ts::Graph_node nq = g.add_node(ts::Named{}, [&seq, &q_order](int&) { q_order.store(++seq); }, q);
    nq.after(np);
    g.compile();

    g.execute().sync();
    TS_CHECK(p_order.load() == 1);
    TS_CHECK(q_order.load() == 2);
}

void test_independent_parallel()
{
    tests::Parallel_gate gate{ 2 };
    ts::Guarded<int> a{ ts::Named{}, 0 }, b{ ts::Named{}, 0 };

    ts::Static_task_graph g;
    auto job = [&gate](int&) { gate.arrive(); };
    g.add_node(ts::Named{}, job, a);
    g.add_node(ts::Named{}, job, b);
    g.compile();

    g.execute().sync();
    TS_CHECK(gate.met());   // no conflict -> nodes run concurrently
}

void test_re_run_counts()
{
    ts::Guarded<int> a{ ts::Named{}, 0 };
    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](int& v) { ++v; }, a);
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
    ts::Guarded<int> a{ ts::Named{}, 0 };
    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](int& v) { v = 1; }, a);
    g.compile();
    g.execute().sync();
    TS_CHECK(read_value(a) == 1);
}

void test_diamond()
{
    tests::Parallel_gate gate{ 2 };
    ts::Guarded<int> x{ ts::Named{}, 0 }, y{ ts::Named{}, 0 }, z{ ts::Named{}, 0 }, w{ ts::Named{}, 0 };

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](int& v) { v = 1; }, x);
    g.add_node(ts::Named{}, [&gate](const int& xv, int& yv) { gate.arrive(); yv = xv + 1; }, x, y);
    g.add_node(ts::Named{}, [&gate](const int& xv, int& zv) { gate.arrive(); zv = xv + 2; }, x, z);
    g.add_node(ts::Named{}, [](const int& yv, const int& zv, int& wv) { wv = yv + zv; }, y, z, w);
    g.compile();

    g.execute().sync();
    TS_CHECK(read_value(w) == 5);    // (1+1) + (1+2)
    TS_CHECK(gate.met());            // the two middle nodes ran concurrently
}

constexpr int wide = 32;

void test_completion_after_all()
{
    auto data = tests::make_guarded_array<int, wide>();
    std::atomic<int> ran{ 0 };

    ts::Static_task_graph g;
    for (auto& d : data)
        g.add_node(ts::Named{}, [&ran](int&) { std::this_thread::sleep_for(1ms); ran.fetch_add(1); }, d);
    g.compile();

    g.execute().sync();
    TS_CHECK(ran.load() == wide);   // get() returned only after every node
}

void test_graph_stress()
{
    auto data = tests::make_guarded_array<int, wide>();
    ts::Static_task_graph g;
    for (auto& d : data)
        g.add_node(ts::Named{}, [](int& v) { ++v; }, d);
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
    auto data = tests::make_guarded_array<int, wide>();
    std::atomic<int> ran{ 0 };

    ts::Static_task_graph g;
    for (auto& d : data)
        g.add_node(ts::Named{}, [&ran](int& v) { ran.fetch_add(1); v = 1; }, d);
    g.compile();

    ts::Cancellation_source src;
    src.request_cancel();               // cancel before running
    ts::Task<void> run = g.execute({ .token = src.token() });
    run.sync();

    TS_CHECK(run.is_cancelled());
    TS_CHECK(ran.load() == 0);          // every node skipped
}

// Cancellation arriving mid-run: the first node cancels the run's token from inside its
// body (this body does not declare the trailing-token parameter - it captures the source it
// was built with). It has already passed its own token check, so it finishes; the second
// node, edge-ordered behind it, sees the cancelled token when it is about to run and settles
// cancelled without running its body; the run's completion settles cancelled.
void test_cancel_mid_run()
{
    ts::Cancellation_source src;
    ts::Guarded<int> x{ ts::Named{}, 0 };

    ts::Static_task_graph g;
    auto first = g.add_node(ts::Named{}, [&src](int& v)
    {
        v += 1;
        src.request_cancel();
    }, x);
    auto second = g.add_node(ts::Named{}, [](int& v) { v += 10; }, x);
    second.after(first);
    g.compile();

    ts::Task<void> run = g.execute({ .token = src.token() });
    run.sync();   // a cancelled void task unblocks normally

    TS_CHECK(run.is_cancelled());
    TS_CHECK(x.async([](const int& v) { return v; }).sync() == 1);   // second's body never ran
}

// The trailing-token opt-in, introspectable tier: a node body declaring a trailing
// `Cancellation_token` after its resource parameters receives the run's token. The body
// proves it is the live token (not a stale capture): clear before the captured source
// cancels, set right after. Its cooperative return settles the node completed - the write
// is visible - while the run's completion settles cancelled (the run token was cancelled).
void test_node_trailing_token()
{
    ts::Cancellation_source src;
    ts::Guarded<int> x{ ts::Named{}, 0 };
    bool clear_before = false, set_after = false;

    ts::Static_task_graph g;
    g.add_node("n", [&](int& v, ts::Cancellation_token t)
    {
        clear_before = !t.is_cancel_requested();
        src.request_cancel();
        set_after = t.is_cancel_requested();   // the delivered token is the run's live token
        v = 7;                                 // cooperative return: the node still completes
    }, x);
    g.compile();

    ts::Task<void> run = g.execute({ .token = src.token() });
    run.sync();

    TS_CHECK(clear_before);
    TS_CHECK(set_after);
    TS_CHECK(run.is_cancelled());              // the run token ended up cancelled
    TS_CHECK(read_value(x) == 7);              // but the body ran to its cooperative return
}

// The trailing-token opt-in, probed (generic-lambda) tier: the rvalue probe must still
// classify the resource positions with the token as an extra probe argument - `auto&` =
// write, `const auto&` = read - proven by the propagated values.
void test_node_trailing_token_probed()
{
    ts::Guarded<int> a{ ts::Named{}, 1 }, b{ ts::Named{}, 0 };

    ts::Static_task_graph g;
    g.add_node("w", [](auto& v, ts::Cancellation_token t)
    {
        if (!t.is_cancel_requested())
            v += 10;
    }, a);
    g.add_node("r", [](const auto& v, auto& out, ts::Cancellation_token) { out = v * 2; }, a, b);
    g.compile();

    g.execute().sync();   // no token passed: the default token never cancels

    TS_CHECK(read_value(a) == 11);   // `auto&` deduced write, token clear so the body wrote
    TS_CHECK(read_value(b) == 22);   // read-then-write chain ordered after the writer
}

// The trailing-token opt-in for a coroutine node body: `Task<void>(T&, Cancellation_token)`
// - the token is copied into the frame and stays valid across suspensions.
void test_node_trailing_token_coroutine()
{
    ts::Guarded<int> x{ ts::Named{}, 0 };

    ts::Static_task_graph g;
    g.add_node("co", [](int& v, ts::Cancellation_token t) -> ts::Task<void>
    {
        if (!t.is_cancel_requested())
            v = 5;
        co_return;
    }, x);
    g.compile();

    g.execute().sync();
    TS_CHECK(read_value(x) == 5);
}

// A node fans out sub-work with parallel_for; the synchronous join gates the body, so the
// run cannot complete until every helper settles.
void test_nested_gates_completion()
{
    constexpr int n = 8;
    ts::Guarded<int> owned{ ts::Named{}, 0 };
    std::atomic<int> done_count{ 0 };

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [&done_count](int&)
    {
        ts::parallel_for(n, [&done_count](int) { done_count.fetch_add(1, std::memory_order_relaxed); });
    }, owned);
    g.compile();

    g.execute().sync();
    TS_CHECK(done_count.load() == n);   // get() returned only after every helper
}

// parallel_for sub-work touches the node's owned guarded object; the helpers inherit the
// node's access grant (Access_context snapshot), so the harness must accept it.
void test_nested_inherits_access()
{
    ts::Guarded<tests::Counter> c{ ts::Named{} };

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](tests::Counter& counter)
    {
        ts::parallel_for(1, [&counter](int) { counter.add(5); });   // guarded write under the inherited grant
    }, c);
    g.compile();

    g.execute().sync();
    int v = c.async([](const tests::Counter& k) { return k.value(); }).sync();
    TS_CHECK(v == 5);
}

// Sub-work gates a conflicting successor: the reader node must see every write the writer
// node's parallel_for made (the synchronous join gates the writer's completion, which
// orders the edge).
void test_nested_before_successor()
{
    constexpr int n = 16;
    ts::Guarded<std::array<int, n>> arr{ ts::Named{} };
    std::atomic<int> sum_seen{ -1 };

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](std::array<int, n>& a)
    {
        ts::parallel_for(n, [&a](int k) { a[k] = k + 1; });   // disjoint elements: no race
    }, arr);
    g.add_node(ts::Named{}, [&sum_seen](const std::array<int, n>& a)
    {
        int s = 0;
        for (int v : a) s += v;
        sum_seen.store(s);
    }, arr);
    g.compile();

    g.execute().sync();
    TS_CHECK(sum_seen.load() == n * (n + 1) / 2);   // reader ran after every write
}

// Per-node priority: a writer root gates three reader successors, released together when
// it completes; on a single worker they run in priority order. Deterministic because the
// root queues all three (in its node_complete) before the worker picks the next task.
void test_node_priority_order()
{
    ts::Scheduler_scope s{ { .num_workers = 1 } };
    ts::Guarded<int> a{ ts::Named{}, 0 };
    std::atomic<int> seq{ 0 };
    std::atomic<int> high_ord{ 0 }, normal_ord{ 0 }, low_ord{ 0 };

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](int& v) { v = 1; }, a);   // writer root -> the three readers run after it
    g.add_node(ts::Named{}, [&seq, &low_ord](const int&) { low_ord.store(seq.fetch_add(1)); }, a).set_priority(ts::Priority::low);
    g.add_node(ts::Named{}, [&seq, &normal_ord](const int&) { normal_ord.store(seq.fetch_add(1)); }, a).set_priority(ts::Priority::normal);
    g.add_node(ts::Named{}, [&seq, &high_ord](const int&) { high_ord.store(seq.fetch_add(1)); }, a).set_priority(ts::Priority::high);
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
    ts::Guarded<int> x{ ts::Named{}, 0 };
    std::atomic<std::thread::id> node_thread{};

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [&node_thread](int& v) { node_thread.store(std::this_thread::get_id()); v = 1; }, x).set_inline();
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
    ts::Guarded<int> x{ ts::Named{}, 0 };
    std::atomic<int> seq{ 0 };
    std::atomic<int> a_ord{ 0 }, b_ord{ 0 };
    std::atomic<std::thread::id> a_thr{}, b_thr{};

    ts::Static_task_graph g;
    ts::Graph_node a = g.add_node(ts::Named{}, [&](int& v) { a_ord.store(++seq); a_thr.store(std::this_thread::get_id()); v = 1; }, x).set_inline();
    ts::Graph_node b = g.add_node(ts::Named{}, [&](int& v) { b_ord.store(++seq); b_thr.store(std::this_thread::get_id()); v = 2; }, x).set_inline();
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
    ts::Guarded<int> x{ ts::Named{}, 0 };
    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](int& v) { ++v; }, x).set_inline();
    g.compile();
    for (int i = 0; i < 5; ++i)
        g.execute().sync();
    TS_CHECK(read_value(x) == 5);
}

// The DOT structure dump (`compile(DOT_path)`): named / unnamed labels, edge colour by
// provenance (green explicit / cyan derived) with a conflict tooltip on the derived, the
// guarded-object list, and the per-node access numbers in label + tooltip.
void test_dot_dump()
{
    ts::Guarded<int> x{ ts::Named{"counter"}, 0 };   // literal -> tooltip uses the name
    ts::Guarded<int> y{ ts::Named{}, 0 };            // call site -> `file:line`

    ts::Static_task_graph g;
    g.add_node("writer_a", [](int& v) { v = 1; }, x);
    ts::Graph_node b = g.add_node("reader_b", [](const int& v, int& w) { w = v; }, x, y);
    ts::Graph_node c = g.add_node(ts::Named{}, [](const int& w) { (void)w; }, y);   // site-labelled
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
    TS_CHECK(dot.find("legend [shape=none") != std::string::npos);
    TS_CHECK(dot.find("guarded objects") != std::string::npos);   // the object-list table
    TS_CHECK(dot.find("writer_a") != std::string::npos);
    TS_CHECK(dot.find("reader_b") != std::string::npos);
    // The `ts::Named{}` node and object are labelled by their call site, not an ordinal.
    TS_CHECK(dot.find("graph_tests.cpp:") != std::string::npos);
    // a->b: derived from the x conflict (writer -> reader), cyan + tooltip with x's `ts::Named` name
    TS_CHECK(dot.find("n0 -> n1 [color=\"#66d9ef\", penwidth=1.8, tooltip=\"counter: RW->RO\"") != std::string::npos);
    // b->c: explicit (green), tooltip still carries the y conflict - y is site-named
    TS_CHECK(dot.find("n1 -> n2 [color=\"#a6e22e\", penwidth=2.0, tooltip=\"explicit ordering; graph_tests.cpp:")
             != std::string::npos);
    TS_CHECK(dot.find(": RW->RO\"") != std::string::npos);
    // node tooltips decode the access numbers: writer_a writes `counter` (object 1)
    TS_CHECK(dot.find("tooltip=\"writer_a\\n1: counter - read/write\"") != std::string::npos);

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
    ts::Guarded<int> y{ ts::Named{}, 0 };

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
    ts::Guarded<int> x{ ts::Named{}, 0 };
    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](int& v) { ++v; }, x);
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
    ts::Guarded<int> x{ ts::Named{}, 0 };

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
    ts::Guarded<int> x{ ts::Named{}, 0 }, y{ ts::Named{}, 0 };

    ts::Static_task_graph g;
    g.add_node("hi", [](int& v) { ++v; }, x).set_priority(ts::Priority::high);
    g.add_node("lo", [](int& v) { ++v; }, y).set_priority(ts::Priority::low);
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
    // (with a text-anchor) or outside (without), depending on measured widths - accept
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

// Handoff weld + dead-time + utilization rendering, on synthetic folds: `on_run_complete`
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
    TS_CHECK(svg.find("critical path dead time") != std::string::npos);   // the headline line (now a hoverable term)
    TS_CHECK(svg.find("core utilization") != std::string::npos);
    TS_CHECK(std::abs(trace.core_utilization() - 770.0 / 1400.0) < 1e-9);  // exact arithmetic
    TS_CHECK(ts::tools::dead_time_ok_share < ts::tools::dead_time_bad_share);   // band order
    TS_CHECK(ts::tools::core_util_ok_share < ts::tools::core_util_good_share);  // band order
    TS_CHECK(svg.find(">W0<") == std::string::npos);   // rows are anonymous - no worker labels
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
// fold a nonzero utilization (the loose floor guards the arm/stamp/fold chain - exact
// figures are the synthetic test's job). Includes the in-flight case: the fold runs
// inside the settling worker's task, so only in-flight-aware busy accounting sees the
// window's work at all.
void test_graph_trace_end_to_end_utilization()
{
    ts::Guarded<int> x{ ts::Named{}, 0 };
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
    ts::Scheduler_scope one{ { .num_workers = 1 } };
    for (int i = 0; i < 8; ++i)
        g.execute().sync();
    g.set_trace(nullptr);

    TS_CHECK(trace.run_count() == 8);
    TS_CHECK(trace.core_utilization() > 0.3);
    TS_CHECK(trace.core_utilization() <= 1.0);
}

// Task volume: the trace's task counter sees every task the scheduler runs - the node, the
// helpers a `parallel_for` submits, anything launched from the body - not just the node count.
// The assertion is anchored on tasks the test itself dispatches and awaits: the node launches
// `extra` tasks through the scheduler and `co_await`s every one before returning, so each
// passes `run_task` inside the run's armed window and is counted, and every run contributes at
// least 1 + `extra`. The `parallel_for` stays because the stat exists to expose that fan-out,
// but it is not what the check rests on. Its `conc - 1` helpers are submitted unconditionally,
// yet the caller's `parallel_for` returns once the *items* are done, not once the helpers have
// exited - a helper that got its worker late claims nothing and runs `run_task` after the node
// has completed and the trace has disarmed, so it is not counted. Under load most helpers are
// late, the count drops toward the run count, and a check that depended on it asserted the
// OS scheduler's timing rather than the counter.
void test_graph_trace_task_count()
{
    ts::Guarded<int> x{ ts::Named{}, 0 };
    auto busy = [](int us)
    {
        auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
        while (std::chrono::steady_clock::now() < until) {}
    };
    constexpr int extra = 8;

    ts::Static_task_graph g;
    g.add_node("fanout", [busy](int& v) -> ts::Task<void>
    {
        ts::parallel_for(64, [&busy](int) { busy(30); });   // fans out onto the pool, opportunistically
        ts::Task<void> launched[extra];
        for (ts::Task<void>& t : launched)
            t = ts::launch([busy] { busy(10); });           // each one a scheduled task, counted
        for (ts::Task<void>& t : launched)
            co_await t;
        ++v;
    }, x);
    g.compile();

    ts::tools::Graph_trace trace;
    g.set_trace(&trace);
    ts::Scheduler_scope pool{ { .num_workers = 4 } };
    constexpr int N = 6;
    for (int i = 0; i < N; ++i)
        g.execute().sync();
    g.set_trace(nullptr);

    TS_CHECK(trace.run_count() == N);
    TS_CHECK(trace.task_total() >= static_cast<long long>(N) * (1 + extra));   // node + the launches, every run
    TS_CHECK(trace.tasks_per_run() >= 1.0 + extra);
}

// Task-system overhead metric on synthetic folds: `on_run_complete` derives machinery by pure
// subtraction, M = busy - B, and `overhead()` = M / (B + M). Deterministic arithmetic.
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
    // busy 1000 us, body 900 us -> machinery M = busy - B = 100 us -> overhead exactly 0.10.
    for (int i = 0; i < 8; ++i)
    {
        trace.on_run_complete(readys, starts, ends, workers, 1, 0, ticks(1000), ticks(1000), 1,
            nullptr, 0, 0, nullptr, 1, ticks(900), 0);
    }

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
    TS_CHECK(svg.find("framework overhead") != std::string::npos);   // the headline figure
}

// End-to-end: a real graph on a real pool must fold nonzero body and machinery, with a
// sane overhead share (bodies dominate at this granularity, so it stays well below 1).
void test_graph_trace_overhead_end_to_end()
{
    ts::Guarded<int> x{ ts::Named{}, 0 };
    auto busy = [](int us)
    {
        auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
        while (std::chrono::steady_clock::now() < until) {}
    };

    ts::Static_task_graph g;
    // Fan out heavily: each node's in-body `parallel_for` submits many slice tasks, each a real
    // task with its own run_task span and a find_work dispatch scan, so `M = busy - B`
    // (setup/completion + dispatch, everything in busy that is not the slice body) accrues over
    // many events and is comfortably positive on a normal run. The machinery/overhead assertions
    // below are `>= 0`, not `> 0`: `M` is >= 0 by construction (the body span nests inside the
    // run_task span), and a loaded runner can leave the per-slice machinery below the clock tick,
    // folding `M` to exactly 0 - a measurement floor, not a regression. The load-bearing checks are
    // that the body is measured and the overhead share stays bounded.
    g.add_node("ov_a", [busy](int& v) { ts::parallel_for(64, [&busy](int) { busy(20); }); ++v; }, x);
    g.add_node("ov_b", [busy](int& v) { ts::parallel_for(64, [&busy](int) { busy(20); }); ++v; }, x);
    g.compile();

    ts::tools::Graph_trace trace;
    g.set_trace(&trace);
    ts::Scheduler_scope pool{ { .num_workers = 4 } };
    for (int i = 0; i < 8; ++i)
        g.execute().sync();
    g.set_trace(nullptr);

    TS_CHECK(trace.run_count() == 8);
    TS_CHECK(trace.body_us() > 0.0);          // user compute measured (the busy() spins)
    TS_CHECK(trace.machinery_us() >= 0.0);    // scheduler cost; 0 only when it rounds below the clock
    TS_CHECK(trace.overhead() >= 0.0);
    TS_CHECK(trace.overhead() < 0.5);         // bodies dwarf the fan-out machinery at this granularity
}

void test_death_cycle()            { TS_CHECK(ts::test::expect_death("graph_cycle")); }
void test_death_before_compile()   { TS_CHECK(ts::test::expect_death("execute_before_compile")); }
void test_death_compile_twice()    { TS_CHECK(ts::test::expect_death("graph_compile_twice")); }
void test_death_add_after_compile(){ TS_CHECK(ts::test::expect_death("graph_add_node_after_compile")); }
void test_death_duplicate_object() { TS_CHECK(ts::test::expect_death("graph_duplicate_object")); }
void test_death_undeclared()       { TS_CHECK(ts::test::expect_death("graph_undeclared")); }
void test_death_guarded_outlived() { TS_CHECK(ts::test::expect_death("guarded_outlived_by_graph")); }
void test_death_graph_mid_run()    { TS_CHECK(ts::test::expect_death("graph_destroyed_mid_run")); }
// The sharp same-object diagnostic (a node syncing an access to an object it holds --
// the certain-deadlock shape); subprocess because the child genuinely deadlocks after
// reporting (it aborts once the report is observed).
void test_death_sync_own_object()  { TS_CHECK(ts::test::expect_death("sync_own_object_deadlock")); }

// The pipe-registration counts stay balanced across moves and a move-assign overwrite -
// so destroying the graphs and then the objects raises no lifetime fatal, and every
// configuration still runs correctly.
void test_lifetime_registration_balance()
{
    ts::Guarded<int> a{ ts::Named{}, 0 };
    ts::Guarded<int> b{ ts::Named{}, 0 };
    {
        ts::Static_task_graph g;
        g.add_node(ts::Named{}, [](int& x) { x += 1; }, a);
        g.compile();
        g.execute().sync();

        ts::Static_task_graph g2 = std::move(g);   // registration rides the move
        g2.execute().sync();

        ts::Static_task_graph g3;
        g3.add_node(ts::Named{}, [](int& y) { y += 10; }, b);
        g3.compile();
        g3.execute().sync();
        g3 = std::move(g2);   // overwrite releases g3's registration of `b`
        g3.execute().sync();
    }   // all graphs destroyed -> all registrations released

    TS_CHECK(a.async([](const int& x) { return x; }).sync() == 3);   // three runs of the writer
    TS_CHECK(b.async([](const int& y) { return y; }).sync() == 10);
    // `a`/`b` destruct at scope end without a lifetime fatal = the balance held.
}

// --- E: reservations / graph handoff (pipe-rebase regression guard) --------

bool probe_ok(ts::Guarded<tests::Rw_probe>& p)
{
    return p.async([](const tests::Rw_probe& r) { return !r.violated(); }).sync();
}

int probe_writes(ts::Guarded<tests::Rw_probe>& p)
{
    return p.async([](const tests::Rw_probe& r) { return r.writes(); }).sync();
}

// E1: a serial chain of writer nodes on one object - each hands the object directly to
// its successor (conflict edges order them; the handoff elides release + re-acquire). The
// oracle must see no reader/writer overlap and every write applied, across re-runs.
void test_writer_handoff_chain()
{
    ts::Guarded<tests::Rw_probe> probe{ ts::Named{} };
    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](tests::Rw_probe& p) { p.observe_write(1); }, probe);
    g.add_node(ts::Named{}, [](tests::Rw_probe& p) { p.observe_write(2); }, probe);
    g.add_node(ts::Named{}, [](tests::Rw_probe& p) { p.observe_write(3); }, probe);
    g.compile();

    g.execute().sync();
    TS_CHECK(probe_ok(probe));
    TS_CHECK(probe_writes(probe) == 3);

    g.execute().sync();   // re-run: the handoff path is allocation-free and repeatable
    TS_CHECK(probe_ok(probe));
    TS_CHECK(probe_writes(probe) == 6);
}

// E2: a graph reader node and a concurrent async read on the same object overlap - the
// per-node mode-aware hold joins concurrent readers. The gate is met only if both were in
// flight at once (the async reader blocks in arrive(), holding its reader slot, while the
// node acquires the pipe as a second concurrent reader).
void test_reader_node_overlaps_async()
{
    tests::Parallel_gate gate{ 2 };
    ts::Guarded<int> x{ ts::Named{}, 5 };

    ts::Task<int> a = x.async([&gate](const int& v) { gate.arrive(); return v; });

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [&gate](const int& v) { gate.arrive(); (void)v; }, x);
    g.compile();
    ts::Task<void> run = g.execute();

    a.sync();
    run.sync();
    TS_CHECK(gate.met());
}

// --- Nested graph runs (docs/coroutine-first.md §4.8) ----------------------

// An outer node holds a write grant on `x` and awaits an inner graph that
// also writes `x`. Without lending the inner node's pipe turn queues behind the outer node's
// own hold - which the outer cannot release, because it is suspended waiting for the inner
// run - so this test hangs rather than fails if the lend regresses. With lending the inner
// node skips the turn and runs under the outer's grant.
void test_nested_run_lends_write_grant()
{
    ts::Guarded<int> x{ ts::Named{}, 0 };

    ts::Static_task_graph inner;
    inner.add_node(ts::Named{}, [](int& v) { v += 10; }, x);
    inner.compile();

    ts::Static_task_graph outer;
    outer.add_node(ts::Named{}, [&inner](int& v) -> ts::Task<void>
    {
        v += 1;                          // under the outer node's write grant
        co_await inner.execute();        // lent: the inner writer skips its turn on `x`
        v += 100;                        // still granted after the inner run
    }, x);
    outer.compile();

    outer.execute().sync();
    TS_CHECK(read_value(x) == 111);

    outer.execute().sync();   // re-run: the link slab rebinds cleanly both ways
    TS_CHECK(read_value(x) == 222);
}

// A lent object still orders the inner nodes among themselves: skipping the pipe turn drops
// only the outer world's exclusion, never the compiled conflict edges. Three inner writers
// on the lent object must apply in declaration order.
void test_nested_run_inner_edges_survive_lend()
{
    ts::Guarded<std::string> log{ ts::Named{} };

    ts::Static_task_graph inner;
    inner.add_node(ts::Named{}, [](std::string& s) { s += "a"; }, log);
    inner.add_node(ts::Named{}, [](std::string& s) { s += "b"; }, log);
    inner.add_node(ts::Named{}, [](std::string& s) { s += "c"; }, log);
    inner.compile();

    ts::Static_task_graph outer;
    outer.add_node(ts::Named{}, [&inner](std::string& s) -> ts::Task<void>
    {
        s += "[";
        co_await inner.execute();
        s += "]";
    }, log);
    outer.compile();

    outer.execute().sync();
    TS_CHECK(log.async([](const std::string& s) { return s; }).sync() == "[abc]");
}

// A nested run over objects the caller does not hold takes its turns normally - lending is
// an intersection, not a blanket bypass. The inner graph's write must serialize against a
// concurrent async on the same object (the oracle would flag an overlap).
void test_nested_run_without_overlap_takes_turns()
{
    ts::Guarded<int> held{ ts::Named{}, 0 };
    ts::Guarded<tests::Rw_probe> other{ ts::Named{} };

    ts::Static_task_graph inner;
    inner.add_node(ts::Named{}, [](tests::Rw_probe& p) { p.observe_write(1); }, other);
    inner.compile();

    ts::Static_task_graph outer;
    outer.add_node(ts::Named{}, [&inner](int& v) -> ts::Task<void>
    {
        v = 7;
        co_await inner.execute();
    }, held);
    outer.compile();

    ts::Task<void> hammer = other.async([](const tests::Rw_probe& p) { p.observe_read(2); });
    outer.execute().sync();
    hammer.sync();

    TS_CHECK(read_value(held) == 7);
    TS_CHECK(probe_ok(other));
    TS_CHECK(probe_writes(other) == 1);
}

// An un-awaited inner run joins the caller's scope by default, so the outer run's completion
// gates on it: after `outer.execute().sync()` the inner work has already happened. (Without
// the auto-join the inner run would float and the read below would race it.)
void test_nested_run_auto_joins_scope()
{
    ts::Guarded<int> x{ ts::Named{}, 0 };
    std::atomic<int> inner_done{ 0 };

    ts::Static_task_graph inner;
    inner.add_node(ts::Named{}, [&inner_done](int& v) { std::this_thread::sleep_for(20ms); v += 5; inner_done.fetch_add(1); }, x);
    inner.compile();

    ts::Static_task_graph outer;
    outer.add_node(ts::Named{}, [&inner](int& v)
    {
        v += 1;
        (void)inner.execute();   // handle deliberately dropped: the scope join is what keeps it honest
    }, x);
    outer.compile();

    outer.execute().sync();
    TS_CHECK(inner_done.load() == 1);
    TS_CHECK(read_value(x) == 6);
}

// A detached run opts out of both defaults: it does not gate the caller, and it receives no
// lend - so it takes its turns and simply queues behind the caller's hold, finishing after
// the outer node releases. Awaited from the blue boundary afterwards.
void test_nested_run_detached()
{
    ts::Guarded<int> x{ ts::Named{}, 0 };

    ts::Static_task_graph inner;
    inner.add_node(ts::Named{}, [](int& v) { v += 5; }, x);
    inner.compile();

    ts::Task<void> detached;
    ts::Static_task_graph outer;
    outer.add_node(ts::Named{}, [&inner, &detached](int& v)
    {
        v += 1;
        detached = inner.execute({ .detach = true });   // queues behind this node's own hold
    }, x);
    outer.compile();

    outer.execute().sync();
    detached.sync();
    TS_CHECK(read_value(x) == 6);
}

// Recursion: a grand-inner graph intersects against its caller's context, which already
// carries the lent entries, so the lend composes without extra machinery.
void test_nested_run_recursive()
{
    ts::Guarded<int> x{ ts::Named{}, 0 };

    ts::Static_task_graph deepest;
    deepest.add_node(ts::Named{}, [](int& v) { v += 100; }, x);
    deepest.compile();

    ts::Static_task_graph middle;
    middle.add_node(ts::Named{}, [&deepest](int& v) -> ts::Task<void>
    {
        v += 10;
        co_await deepest.execute();
    }, x);
    middle.compile();

    ts::Static_task_graph outer;
    outer.add_node(ts::Named{}, [&middle](int& v) -> ts::Task<void>
    {
        v += 1;
        co_await middle.execute();
    }, x);
    outer.compile();

    outer.execute().sync();
    TS_CHECK(read_value(x) == 111);
}

// Companion to the mode-incompatible fatal: the sanctioned form is for the calling node to
// declare the write, which then covers the inner graph's writer.
void test_nested_run_write_covers_inner_write()
{
    ts::Guarded<int> x{ ts::Named{}, 0 };

    ts::Static_task_graph inner;
    inner.add_node(ts::Named{}, [](int& v) { v += 2; }, x);
    inner.compile();

    ts::Static_task_graph outer;
    outer.add_node(ts::Named{}, [&inner](int& v) -> ts::Task<void>   // write, not const& - covers the inner write
    {
        co_await inner.execute();
        v += 1;
    }, x);
    outer.compile();

    outer.execute().sync();
    TS_CHECK(read_value(x) == 3);
}

// Companion to the non-quiet-scope fatal: await the earlier nested run first, then lend the
// next. Both runs lend `x` from the outer node's write grant; awaiting the first quiesces the
// frame's scope, so the second lends cleanly.
void test_nested_run_await_previous_then_lend()
{
    ts::Guarded<int> x{ ts::Named{}, 0 };

    ts::Static_task_graph first;
    first.add_node(ts::Named{}, [](int& v) { v += 1; }, x);
    first.compile();

    ts::Static_task_graph second;
    second.add_node(ts::Named{}, [](int& v) { v += 100; }, x);
    second.compile();

    ts::Static_task_graph outer;
    outer.add_node(ts::Named{}, [&first, &second](int& v) -> ts::Task<void>
    {
        (void)v;
        co_await first.execute();    // quiesce: awaited, settles
        co_await second.execute();   // scope quiet -> lend is clean
    }, x);
    outer.compile();

    outer.execute().sync();
    TS_CHECK(read_value(x) == 101);
}

// Two different graphs over overlapping objects, running concurrently. task-internals §10
// scenario 2 called this unsupported, but the reason was the per-graph `Run_state` being
// single-run - which says nothing about two graphs. Each graph's nodes take their pipe turns
// in canonical (pipe-address) order over the same address-sorted objects, so no wait cycle
// can form; the pipe serializes the two graphs' conflicting nodes against each other exactly
// as it serializes a node against an async. The Rw_probe oracle catches any overlap the
// harness structurally cannot (both sides declare their access).
void test_concurrent_graphs_shared_objects()
{
    constexpr int rounds = 60;
    ts::Guarded<tests::Rw_probe> shared_a{ ts::Named{ "shared_a" } }, shared_b{ ts::Named{ "shared_b" } };

    ts::Static_task_graph g1;
    g1.add_node(ts::Named{}, [](tests::Rw_probe& p) { p.observe_write(1); }, shared_a);
    g1.add_node(ts::Named{}, [](const tests::Rw_probe& p) { p.observe_read(2); }, shared_b);
    g1.compile();

    // Opposite declaration order on the same two objects: the canonical order is the pipes'
    // addresses, not the declaration order, so this cannot invert anyone's acquisition.
    ts::Static_task_graph g2;
    g2.add_node(ts::Named{}, [](tests::Rw_probe& p) { p.observe_write(3); }, shared_b);
    g2.add_node(ts::Named{}, [](tests::Rw_probe& p, const tests::Rw_probe& q) { p.observe_write(4); (void)q; },
                shared_a, shared_b);
    g2.compile();

    {
        std::jthread second([&]
        {
            for (int i = 0; i < rounds; ++i)
                g2.execute().sync();
        });
        for (int i = 0; i < rounds; ++i)
            g1.execute().sync();
    }   // join

    TS_CHECK(probe_ok(shared_a));
    TS_CHECK(probe_ok(shared_b));
    TS_CHECK(probe_writes(shared_a) == 2 * rounds);   // g1's writer + g2's multi-object writer
    TS_CHECK(probe_writes(shared_b) == rounds);
}

// Worker-less: every submit executes inline on the submitting thread, so a nested run
// unwinds through the serial trampoline inside the outer node's body. Depth is bounded by
// the nesting, and the lend is what keeps it from self-deadlocking on the outer's own hold.
void test_nested_run_worker_less()
{
    ts::Scheduler_scope scope{ ts::Scheduler_config{ .single_threaded = true } };
    ts::Guarded<int> x{ ts::Named{}, 0 };

    ts::Static_task_graph inner;
    inner.add_node(ts::Named{}, [](int& v) { v += 10; }, x);
    inner.compile();

    ts::Static_task_graph outer;
    outer.add_node(ts::Named{}, [&inner](int& v) -> ts::Task<void>
    {
        v += 1;
        co_await inner.execute();
        v += 100;
    }, x);
    outer.compile();

    outer.execute().sync();
    TS_CHECK(read_value(x) == 111);
}

void test_death_nested_run_mode_conflict() { TS_CHECK(ts::test::expect_death("graph_lend_mode_conflict")); }
void test_death_nested_run_unquiet_scope() { TS_CHECK(ts::test::expect_death("graph_lend_unquiet_scope")); }
void test_death_execute_in_flight()        { TS_CHECK(ts::test::expect_death("graph_execute_in_flight")); }

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
    run("writer handoff chain", test_writer_handoff_chain);
    run("reader node overlaps async", test_reader_node_overlaps_async);
    run("re-run counts", test_re_run_counts);
    run("empty graph", test_empty_graph);
    run("single node", test_single_node);
    run("diamond", test_diamond);
    run("completion after all", test_completion_after_all);
    run("graph stress", test_graph_stress);
    run("cancel skips nodes", test_cancel_skips_nodes);
    run("cancel mid-run skips the rest", test_cancel_mid_run);
    run("node trailing token", test_node_trailing_token);
    run("node trailing token probed", test_node_trailing_token_probed);
    run("node trailing token coroutine", test_node_trailing_token_coroutine);
    run("nested gates completion", test_nested_gates_completion);
    run("nested inherits access", test_nested_inherits_access);
    run("nested before successor", test_nested_before_successor);
    run("node priority order", test_node_priority_order);
    run("graph node inline on caller", test_graph_node_inline_on_caller);
    run("graph inline chain on caller", test_graph_inline_chain_on_caller);
    run("graph inline rerun", test_graph_inline_rerun);
    run_if(with_profiling, "TS_PROFILING=0", "dot dump", test_dot_dump);
    run_if(with_profiling, "TS_PROFILING=0", "graph trace", test_graph_trace);
    run_if(with_profiling, "TS_PROFILING=0", "graph trace cancelled run", test_graph_trace_cancelled);
    run_if(with_profiling, "TS_PROFILING=0", "graph trace critical path", test_graph_trace_critical);
    run_if(with_profiling, "TS_PROFILING=0", "graph trace priority", test_graph_trace_priority);
    run_if(with_profiling, "TS_PROFILING=0", "graph trace weld + dead time", test_graph_trace_weld_dead_time);
    run_if(with_profiling, "TS_PROFILING=0", "graph trace row packing", test_graph_trace_row_packing);
    run_if(with_profiling, "TS_PROFILING=0", "graph trace end-to-end utilization", test_graph_trace_end_to_end_utilization);
    run_if(with_profiling, "TS_PROFILING=0", "graph trace task count", test_graph_trace_task_count);
    run_if(with_profiling, "TS_PROFILING=0", "graph trace overhead", test_graph_trace_overhead);
    run_if(with_profiling, "TS_PROFILING=0", "graph trace overhead end-to-end", test_graph_trace_overhead_end_to_end);
    run("nested run lends the write grant", test_nested_run_lends_write_grant);
    run("nested run: inner edges survive the lend", test_nested_run_inner_edges_survive_lend);
    run("nested run without overlap takes turns", test_nested_run_without_overlap_takes_turns);
    run("nested run auto-joins the caller's scope", test_nested_run_auto_joins_scope);
    run("nested run detached", test_nested_run_detached);
    run("nested run recursive", test_nested_run_recursive);
    run("nested run: outer write covers inner write", test_nested_run_write_covers_inner_write);
    run("nested run: await previous, then lend", test_nested_run_await_previous_then_lend);
    run("nested run worker-less", test_nested_run_worker_less);
    run("concurrent graphs, shared objects", test_concurrent_graphs_shared_objects);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "death: nested run mode conflict", test_death_nested_run_mode_conflict);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "death: nested run with an unquiet scope", test_death_nested_run_unquiet_scope);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "death: execute while a run is in flight", test_death_execute_in_flight);
    run("death: cycle", test_death_cycle);
    run("death: execute before compile", test_death_before_compile);
    run("death: compile twice (build-once)", test_death_compile_twice);
    run("death: add_node after compile (build-once)", test_death_add_after_compile);
    run("death: duplicate object on one node", test_death_duplicate_object);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "death: undeclared access", test_death_undeclared);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "death: Guarded outlived by graph", test_death_guarded_outlived);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "death: graph destroyed mid-run", test_death_graph_mid_run);
    run_if(with_rule_in_task_sync, "TS_RULE_IN_TASK_SYNC off", "death: sync own object (sharp diagnostic)", test_death_sync_own_object);
    run("lifetime registration balance", test_lifetime_registration_balance);
}
