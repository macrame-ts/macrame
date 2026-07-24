// ThreadSanitizer stress driver. Exercises the concurrency paths (scheduler,
// Guarded reader/writer pipe, Static_task_graph + parallel_for, then/when_all)
// without the Windows-specific test harness, so it builds under clang/libstdc++
// with -fsanitize=thread on Linux/macOS. See tsan/run.sh and tsan/README.md.
//
// Run after every major change: a clean exit means TSan found no data races in
// these workloads; a race prints a report and (with halt_on_error) exits nonzero.

#include <cstddef>

// Single-file samples (no headers) -- see sample/game_frame.cpp,
// sample/physics.cpp, sample/blackboard.cpp.
namespace sample
{
void game_frame_stats(int frames, float time_scale,
                      double& avg_ms, double& serial_ms, float& transform0);
void stress_game_frame_optimised(int frames, int workers);
void run_blackboard_sample();
std::size_t physics_pose_hash(int frames);   // final snapshot hash after `frames` frames
}
#include "ts/parallel_for.h"
#include "ts/scheduler.h"
#include "ts/static_task_graph.h"
#include "ts/guarded.h"
#include "ts/deferred.h"
#include "ts/versioned.h"
#include "graph_trace.h"   // tools/: worker-side stamps + settle-side fold under TSan

#if defined(__cpp_impl_coroutine)
#include "ts/coroutine_support.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace
{

void inc(void* p)
{
    static_cast<std::atomic<int>*>(p)->fetch_add(1, std::memory_order_relaxed);
}

// Many external producers hammering one scheduler concurrently.
void stress_scheduler()
{
    constexpr int producers = 8, per = 5000;
    ts::Scheduler s;
    std::atomic<int> done{ 0 };
    {
        std::vector<std::jthread> ps;
        for (int i = 0; i < producers; ++i)
            ps.emplace_back([&] { for (int k = 0; k < per; ++k) s.submit(inc, &done); });
    }   // join producers
    while (done.load(std::memory_order_acquire) < producers * per)
        std::this_thread::yield();
    assert(done.load() == producers * per);
}

// Many external threads issuing concurrent reads and writes to one object: the
// reader/writer pipe must serialize writes and parallelize reads without racing.
void stress_thread_safe()
{
    constexpr int threads = 8, per = 2000;
    ts::Guarded<int> obj{ 0 };
    std::atomic<int> reads{ 0 };
    {
        std::vector<std::jthread> producers;
        for (int t = 0; t < threads; ++t)
            producers.emplace_back([&]
            {
                for (int k = 0; k < per; ++k)
                {
                    if (k & 1)
                        obj.async([](int& v) { ++v; });
                    else
                        obj.async([&reads](const int& v) { reads.fetch_add(1, std::memory_order_relaxed); return v; });
                }
            });
    }   // join producers
    int final = obj.async([](const int& v) { return v; }).sync();   // FIFO drain
    assert(final == threads * per / 2);
}

// Concurrent access on one object mixing `access` (inline-when-free) and `async` (always
// enqueued) jobs across threads. An `access` job runs on the calling thread when the pipe is
// momentarily free, else defers to the queue; either way every increment must land exactly once
// and the pipe must drain. Stresses pipe_try_inline's acquire/run/release racing pipe_enqueue.
void stress_inline_async()
{
    constexpr int threads = 8, per = 2000;
    ts::Guarded<int> obj{ 0 };
    std::atomic<int> reads{ 0 };
    {
        std::vector<std::jthread> producers;
        for (int t = 0; t < threads; ++t)
            producers.emplace_back([&obj, &reads]
            {
                for (int k = 0; k < per; ++k)
                {
                    bool opportunistic = (k % 3) == 0;   // some inline-when-free, some always enqueued
                    if (k & 1)
                    {
                        auto w = [](int& v) { ++v; };
                        if (opportunistic) obj.access(w); else obj.async(w);
                    }
                    else
                    {
                        auto r = [&reads](const int& v)
                            { reads.fetch_add(1, std::memory_order_relaxed); return v; };
                        if (opportunistic) obj.access(r); else obj.async(r);
                    }
                }
            });
    }   // join producers
    int final = obj.async([](const int& v) { return v; }).sync();   // FIFO drain
    assert(final == threads * per / 2);
}

// Many threads triggering one Signal concurrently while others wait / attach
// continuations: idempotent `complete()` must fire exactly once with no race.
void stress_signal()
{
    constexpr int rounds = 2000, triggerers = 6;
    for (int r = 0; r < rounds; ++r)
    {
        ts::Signal sig;
        std::atomic<int> fired{ 0 };
        // Capture the continuation as a task and join on IT: `sig.sync()` waits for the signal
        // to settle, NOT for its downstream `then` continuation (which runs after `settle`'s
        // `notify_all`), so asserting `fired` right after `sig.sync()` would be an over-assertion.
        ts::Task<void> fired_done = sig.then([&fired] { fired.fetch_add(1, std::memory_order_relaxed); });

        {
            std::vector<std::jthread> threads;
            for (int t = 0; t < triggerers; ++t)
                threads.emplace_back([&] { sig.trigger(); });
            threads.emplace_back([&] { sig.sync(); });   // a concurrent waiter
        }   // join
        sig.sync();
        fired_done.sync();   // the continuation has now run
        assert(fired.load() == 1);
    }
}

// Graph with internal parallel bands, re-executed in a loop -- with an aggregating trace
// attached, so the worker-side stamps and the settle-side fold run under TSan (stamps on
// several workers, fold on the settling one, stats read on the main thread after sync).
void stress_graph()
{
    ts::Guarded<int> a{ 0 }, b{ 0 }, c{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& x) { x = 1; }, a);
    g.add_node([](const int& x, int& y) { y = x * 10; }, a, b);
    g.add_node([](const int& x, const int& y, int& z) { z = x + y; }, a, b, c);
    g.compile();
    ts::tools::Graph_trace trace;
    g.set_trace(&trace);
    for (int i = 0; i < 200; ++i)
        g.execute().sync();
    assert(trace.run_count() == 200);
    assert(trace.node_stats(0).runs == 200);
    g.set_trace(nullptr);
}

// A graph node fans out NESTED tasks over disjoint elements of the object it owns,
// re-executed in a loop with a conflicting reader successor. Stresses the node-as-block
// path: current_task set/restore on a graph node, the execution_flag/num_locks switch,
// add_nested racing nested completion, the node_complete continuation gating the run and
// the successor, and inherited-access-scope reads on the shared array.
void stress_graph_nested()
{
    constexpr int n = 32;
    ts::Guarded<std::array<int, n>> arr{};
    std::atomic<int> sum{ 0 };

    ts::Static_task_graph g;
    g.add_node([](std::array<int, n>& a)
    {
        for (int k = 0; k < n; ++k)
            ts::nested([&a, k] { a[k] = k; });
    }, arr);
    g.add_node([&sum](const std::array<int, n>& a)
    {
        int s = 0;
        for (int v : a) s += v;
        sum.store(s, std::memory_order_relaxed);
    }, arr);
    g.compile();

    for (int i = 0; i < 300; ++i)
    {
        sum.store(-1, std::memory_order_relaxed);
        g.execute().sync();
        assert(sum.load(std::memory_order_relaxed) == n * (n - 1) / 2);
    }
}

// when_all joining prerequisites that complete on different worker threads: mixed
// void + non-void + move-only, consumed apply-style. Exercises the join's remaining
// counter, slot moves, and finish across threads.
void stress_when_all()
{
    ts::Guarded<int> a{ 3 }, b{ 4 }, c{ 5 };
    std::atomic<int> total{ 0 };
    for (int i = 0; i < 3000; ++i)
    {
        ts::Task<void> v = a.async([](int& x) { ++x; });                          // void
        ts::Task<int> r = b.async([](const int& x) { return x; });               // value
        ts::Task<std::unique_ptr<int>> m = c.async([](const int& x) { return std::make_unique<int>(x); }); // move-only

        int s = ts::when_all(v, r, m)
            .then([](int rv, std::unique_ptr<int>& mv) { return rv + *mv; })      // apply-style, void dropped
            .sync();
        total.fetch_add(s, std::memory_order_relaxed);
    }
    assert(total.load() == 3000 * 9);
}

// when_all with a prerequisite cancelled concurrently with the join: request_cancel
// races the prereq's body check and the join's decrement/finish. The join must settle
// exactly once (completed or cancelled) and never stall (the counter must still reach 0).
void stress_when_all_cancel()
{
    ts::Guarded<int> a{ 1 }, b{ 2 }, c{ 3 };
    for (int i = 0; i < 3000; ++i)
    {
        ts::Cancellation_source src;
        ts::Task<int> ta = a.async([](const int& x) { return x; });
        ts::Task<int> tb = b.async([](const int& x) { return x; }, { .token = src.token() });
        ts::Task<int> tc = c.async([](const int& x) { return x; });

        auto j = ts::when_all(ta, tb, tc);
        std::jthread canceller([&] { src.request_cancel(); });   // race the join
        canceller.join();
        while (!j.is_done())                                     // must settle, not stall
            std::this_thread::yield();
    }
}

// Many external threads launching standalone tasks (body-in-block) concurrently,
// each chained and awaited.
void stress_launch()
{
    constexpr int threads = 6, per = 1500;
    std::atomic<int> sum{ 0 };
    {
        std::vector<std::jthread> ps;
        for (int t = 0; t < threads; ++t)
            ps.emplace_back([&]
            {
                for (int k = 0; k < per; ++k)
                {
                    int v = ts::launch([k] { return k; }).then([](int x) { return x + 1; }).sync();
                    sum.fetch_add(v, std::memory_order_relaxed);
                }
            });
    }   // join
    (void)sum;
}

// Prerequisites: prereqs complete on worker threads while `.after()` registers them
// and `.launch()` arms the dependent -- racing add_prerequisite against settle (the
// per-block mutex) and concurrent num_locks decrements from several prereqs.
void stress_prereq()
{
    for (int i = 0; i < 1500; ++i)
    {
        std::atomic<int> done{ 0 };
        auto a = ts::launch([&] { done.fetch_add(1, std::memory_order_relaxed); });
        auto b = ts::launch([&] { done.fetch_add(1, std::memory_order_relaxed); });
        auto c = ts::launch([&] { done.fetch_add(1, std::memory_order_relaxed); });

        std::atomic<int> seen{ -1 };
        ts::task([&] { seen.store(done.load(std::memory_order_relaxed), std::memory_order_relaxed); })
            .after(a, b, c).launch().sync();
        assert(seen.load() == 3);   // dependent ran only after all three prerequisites
    }
}

// Retraction under oversubscription: nested fork-join where the outer tasks saturate
// the workers and block on inner get()s. Retraction runs the un-started inner tasks
// inline -- stressing the `started` claim (retractor vs worker) and inline execution
// racing worker execution and completion.
void stress_retraction()
{
    const int outer = 2 * static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    for (int i = 0; i < 200; ++i)
    {
        std::atomic<int> total{ 0 };
        std::vector<ts::Task<void>> tasks;
        for (int o = 0; o < outer; ++o)
            tasks.push_back(ts::launch([&]
            {
                // Inner chunks + a dependent join; get() the join -> DEEP retraction
                // (retract the chunks, then the join).
                auto a = ts::launch([&] { total.fetch_add(1, std::memory_order_relaxed); });
                auto b = ts::launch([&] { total.fetch_add(1, std::memory_order_relaxed); });
                auto c = ts::launch([&] { total.fetch_add(1, std::memory_order_relaxed); });
                ts::task([] {}).after(a, b, c).launch().sync();
            }));
        for (auto& t : tasks)
            t.sync();
        assert(total.load() == outer * 3);
    }

    // then / when_all retraction: the continuation's completion is callback-driven, but a
    // retraction-hint backlink lets get() run the (retractable) producer inline. Stresses
    // the hint racing the producer's worker dispatch, and the run_state claim (retractor
    // vs worker) through the continuation.
    for (int i = 0; i < 200; ++i)
    {
        std::atomic<int> total{ 0 };
        std::vector<ts::Task<void>> tasks;
        for (int o = 0; o < outer; ++o)
            tasks.push_back(ts::launch([&]
            {
                int r = ts::launch([&] { total.fetch_add(1, std::memory_order_relaxed); return 1; })
                            .then([&](int x) { total.fetch_add(1, std::memory_order_relaxed); return x + 1; })
                            .sync();
                (void)r;
                ts::Task<int> a = ts::launch([&] { total.fetch_add(1, std::memory_order_relaxed); return 1; });
                ts::Task<int> b = ts::launch([&] { total.fetch_add(1, std::memory_order_relaxed); return 2; });
                int s = ts::when_all(a, b).then([](int x, int y) { return x + y; }).sync();
                (void)s;
            }));
        for (auto& t : tasks)
            t.sync();
        assert(total.load() == outer * 4);
    }
}

// Inline dispatch: dependents marked set_inline() run on whatever thread settles their
// prerequisite (a worker, or the get()-thread's retractor). Under oversubscription the
// outer tasks saturate the workers and get() an inline chain, so it runs inline via
// retraction. Stresses the dispatch fork (release -> dispatch_ready), the per-thread
// trampoline, and inline execution racing worker completion + the run_state claim.
void stress_inline()
{
    const int outer = 2 * static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    for (int i = 0; i < 400; ++i)
    {
        std::atomic<int> count{ 0 };
        std::vector<ts::Task<void>> tasks;
        for (int o = 0; o < outer; ++o)
            tasks.push_back(ts::launch([&count]
            {
                auto a = ts::launch([&count] { count.fetch_add(1, std::memory_order_relaxed); });
                auto b = ts::task([&count] { count.fetch_add(1, std::memory_order_relaxed); })
                             .set_inline().after(a).launch();
                auto c = ts::task([&count] { count.fetch_add(1, std::memory_order_relaxed); })
                             .set_inline().after(b).launch();
                c.sync();
            }));
        for (auto& t : tasks)
            t.sync();
        assert(count.load() == outer * 3);
    }
}

// Nested tasks: a parent spawns several nested tasks; the parent must not complete
// until all settle. Stresses the execution_flag mode switch, add_nested's fetch_add
// racing nested completion (release reaching execution_flag), and the body-end
// self-lock drop racing those completions.
void stress_nested()
{
    for (int i = 0; i < 1500; ++i)
    {
        std::atomic<int> count{ 0 };
        ts::launch([&]
        {
            for (int k = 0; k < 4; ++k)
                ts::nested([&] { count.fetch_add(1, std::memory_order_relaxed); });
        }).sync();
        assert(count.load() == 4);   // all nested done before the parent completed
    }
}

// Reusable task: one block/body re-run many times via reset(). Stresses reset()
// (re-arm) racing the completing worker's settle tail -- get() wakes on notify_all
// while the worker is still exiting settle, then the main thread re-arms and re-launches.
void stress_reuse()
{
    std::atomic<int> total{ 0 };
    auto t = ts::task([&total] { return total.fetch_add(1, std::memory_order_relaxed) + 1; });

    int last = 0;
    for (int i = 0; i < 5000; ++i)
    {
        if (i > 0)
            t.reset();
        t.launch();
        last = t.sync();
    }
    assert(last == 5000);

    // Reuse + prerequisites + retraction: get() retracts (runs the prereq then the
    // dependent inline), racing the worker dispatch; reset() re-arms for the next round.
    // Stresses the generation stamp that voids the stale (retraction-duplicate) dispatch.
    std::atomic<int> log{ 0 };
    auto dep = ts::task([&log] { return log.load(std::memory_order_relaxed); });
    for (int i = 1; i <= 3000; ++i)
    {
        if (i > 1)
            dep.reset();
        ts::Task<void> prereq = ts::launch([&log, i] { log.store(i, std::memory_order_relaxed); });
        dep.after(prereq).launch();
        assert(dep.sync() == i);   // ran after THIS round's prerequisite, not a stale run
    }

    // A resettable Signal re-armed between rounds, with concurrent waiters/triggerers.
    for (int r = 0; r < 2000; ++r)
    {
        ts::Signal sig;
        {
            std::vector<std::jthread> threads;
            for (int w = 0; w < 3; ++w)
                threads.emplace_back([&] { sig.sync(); });
            sig.trigger();
        }   // join
        sig.reset();
        assert(!sig.is_done());
    }
}

// Cancellation racing execution: request_cancel (a store) concurrent with the body's
// token check (a load) and with then-propagation; the block must settle exactly once.
void stress_cancel()
{
    ts::Guarded<int> d{ 0 };

    for (int i = 0; i < 2000; ++i)
    {
        ts::Cancellation_source src;
        ts::Task<int> t = d.async([](const int& v) { return v; }, { .token = src.token() });
        ts::Task<int> u = t.then([](int v) { return v + 1; });   // propagates settle either way

        std::jthread canceller([&] { src.request_cancel(); });   // race the body
        canceller.join();

        while (!u.is_done())
            std::this_thread::yield();
        assert(u.is_done());   // settled exactly once, whether completed or cancelled
    }

    // Graph cancellation racing the run.
    ts::Guarded<int> a{ 0 }, b{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { ++v; }, a);
    g.add_node([](const int& x, int& y) { y = x; }, a, b);
    g.compile();
    for (int i = 0; i < 500; ++i)
    {
        ts::Cancellation_source src;
        std::jthread canceller([&] { src.request_cancel(); });
        g.execute(ts::default_scheduler(), src.token()).sync();
        canceller.join();
    }
}

// A token-taking body polling the token while another thread cancels: request_cancel racing
// the body's polling read of the shared cancel flag, and the block settling completed after a
// cooperative early-out. Also races a then-inline continuation dispatched off the completer.
void stress_token_body()
{
    for (int i = 0; i < 2000; ++i)
    {
        ts::Cancellation_source src;
        std::atomic<bool> started{ false };
        std::atomic<int> stage{ 0 };

        ts::Task<int> t = ts::launch([&started, &stage](ts::Cancellation_token tok) -> int
        {
            started.store(true, std::memory_order_relaxed);
            while (!tok.is_cancel_requested())
                std::this_thread::yield();
            stage.store(1, std::memory_order_relaxed);
            return 7;
        }, { .token = src.token() });

        ts::Task<int> u = t.then([](int v) { return v + 1; }, { .run_inline = true });

        while (!started.load(std::memory_order_relaxed))
            std::this_thread::yield();
        src.request_cancel();

        assert(u.sync() == 8);            // body early-outed, completed, continuation ran inline
        assert(stage.load() == 1);
    }

    // Same, through an async accessor (body runs on the pipe): request_cancel races the
    // accessor's polling read.
    ts::Guarded<int> d{ 5 };
    for (int i = 0; i < 2000; ++i)
    {
        ts::Cancellation_source src;
        std::atomic<bool> started{ false };
        ts::Task<int> t = d.async([&started](const int& v, ts::Cancellation_token tok) -> int
        {
            started.store(true, std::memory_order_relaxed);
            while (!tok.is_cancel_requested())
                std::this_thread::yield();
            return v;
        }, { .token = src.token() });

        while (!started.load(std::memory_order_relaxed))
            std::this_thread::yield();
        src.request_cancel();
        assert(t.sync() == 5);
    }

    // A then continuation whose own body takes a token, cancelled while it runs: cancel
    // races the continuation's polling read after it dispatches off the producer.
    for (int i = 0; i < 2000; ++i)
    {
        ts::Cancellation_source src;
        std::atomic<bool> started{ false };
        ts::Task<int> p = ts::launch([] { return 1; });
        ts::Task<int> u = p.then([&started](int v, ts::Cancellation_token tok)
        {
            started.store(true, std::memory_order_relaxed);
            while (!tok.is_cancel_requested())
                std::this_thread::yield();
            return v + 1;
        }, { .token = src.token() });

        while (!started.load(std::memory_order_relaxed))
            std::this_thread::yield();
        src.request_cancel();
        assert(u.sync() == 2);
    }
}

// Cancel callbacks racing request_cancel: several threads register a Cancel_callback that
// lives briefly then destroys, while another thread cancels. Stresses the callback-list
// mutex, the fire-immediately-when-already-requested path, and -- the delicate one -- the
// destructor waiting out a callback that is firing on another thread (no UAF, no hang).
void stress_cancel_callback()
{
    for (int r = 0; r < 3000; ++r)
    {
        ts::Cancellation_source src;
        std::atomic<int> fired{ 0 };
        {
            std::vector<std::jthread> threads;
            for (int t = 0; t < 4; ++t)
                threads.emplace_back([&]
                {
                    ts::Cancel_callback cb(src.token(),
                        [&fired] { fired.fetch_add(1, std::memory_order_relaxed); });
                    std::this_thread::yield();   // let its dtor race the cancel's firing
                });
            threads.emplace_back([&] { src.request_cancel(); });
        }   // join: each callback either fired or was deregistered first; neither races
        (void)fired;
    }
}

// A graph node accessing an object directly while other threads fire async on the
// SAME object: the per-run pipe reservation must keep them from overlapping.
// Per-node mode-aware acquire/release coexisting with out-of-band async. Multi-object
// nodes (incremental canonical-order acquire), a gap node (x free between its accessors),
// and mixed read/write nodes, while async readers AND writers hammer the same objects --
// stresses pipe_acquire/pipe_release (both modes), the acquire callback chain racing async
// completion, and the gap windows. A writer node must never overlap any async on its object;
// concurrent reads may overlap (allowed).
void stress_graph_async()
{
    ts::Guarded<int> x{ 0 }, y{ 0 };
    ts::Static_task_graph g;
    ts::Graph_node n1 = g.add_node([](int& a) { ++a; }, x);                       // write x
    ts::Graph_node n2 = g.add_node([](int& a, int& b) { ++a; ++b; }, x, y);       // write x (handoff from n1) + write y
    ts::Graph_node n3 = g.add_node([](int& a) { ++a; }, x);                       // write x (handoff from n2)
    n2.after(n1);
    n3.after(n2);
    g.compile();

    std::atomic<bool> stop{ false };
    {
        std::vector<std::jthread> firers;
        for (int t = 0; t < 3; ++t)
            firers.emplace_back([&]
            {
                while (!stop.load(std::memory_order_relaxed))
                {
                    x.async([](int& v) { ++v; }).sync();                 // write x
                    x.async([](const int& v) { return v; }).sync();      // read x
                    y.async([](const int& v) { return v; }).sync();      // read y
                }
            });

        for (int i = 0; i < 300; ++i)
            g.execute().sync();

        stop.store(true, std::memory_order_relaxed);
    }   // join firers
}

// Inline graph nodes (set_inline) under async contention: an inline node whose object is
// free at settle runs inline on the settling thread (trampolined for chains); if an async
// grabbed the object in the gap, the acquire defers and the node runs queued. Stresses the
// synchronous-vs-deferred fork in acquire_next, dispatch_ready reused for graph nodes, and
// node_complete releasing while async races to acquire. A chain of three inline nodes on the
// same object exercises the trampoline + the release/re-acquire hand-off.
void stress_graph_inline()
{
    ts::Guarded<int> x{ 0 };
    ts::Static_task_graph g;
    ts::Graph_node a = g.add_node([](int& v) { ++v; }, x).set_inline();
    ts::Graph_node b = g.add_node([](int& v) { ++v; }, x).set_inline();
    ts::Graph_node c = g.add_node([](int& v) { ++v; }, x).set_inline();
    b.after(a);
    c.after(b);
    g.compile();

    std::atomic<bool> stop{ false };
    {
        std::vector<std::jthread> firers;
        for (int t = 0; t < 3; ++t)
            firers.emplace_back([&]
            {
                while (!stop.load(std::memory_order_relaxed))
                    x.async([](int& v) { ++v; }).sync();   // contends the inline nodes' object
            });

        for (int i = 0; i < 300; ++i)
            g.execute().sync();

        stop.store(true, std::memory_order_relaxed);
    }   // join firers
}

// Multi-object async under contention: concurrent multi-object asyncs over the SAME pair in
// opposite declared orders (canonical-order acquire must keep them deadlock-free), mixed with
// single-object asyncs on each. Stresses multi_acquire's chain (immediate + deferred), the
// release-on-completion continuation, and cross-object hold-and-wait (no cycle).
void stress_multi_async()
{
    ts::Guarded<int> a{ 0 }, b{ 0 };
    {
        std::vector<std::jthread> firers;
        for (int t = 0; t < 4; ++t)
            firers.emplace_back([&]
            {
                for (int i = 0; i < 1500; ++i)
                {
                    ts::async([](int& x, int& y) { ++x; ++y; }, a, b).sync();   // declared order a, b
                    ts::async([](int& x, int& y) { ++x; ++y; }, b, a).sync();   // declared order b, a
                    a.async([](const int& v) { return v; }).sync();
                    b.async([](const int& v) { return v; }).sync();
                }
            });
    }   // join firers
}

// Idle policies: the handoff spinner protocol (num_spinning + successor promotion + the
// last-spinner-parks-vs-producer-wake race) and spin_then_block, driven by many concurrent
// producers plus burst/drain cycles that fully park the pool between bursts (exercising the
// 0->1 producer wake). A dedicated scheduler per policy; a counter increment per task.
void stress_idle_policy(ts::Idle_policy policy)
{
    // Sustained multi-producer submit.
    {
        ts::Scheduler s{ { .idle_policy = policy } };
        std::atomic<int> n{ 0 };
        constexpr int per_producer = 20000, producers = 4;
        {
            std::vector<std::jthread> ps;
            for (int p = 0; p < producers; ++p)
                ps.emplace_back([&] { for (int i = 0; i < per_producer; ++i) s.submit(inc, &n); });
        }   // join producers
        while (n.load(std::memory_order_acquire) != per_producer * producers)
            std::this_thread::yield();
        assert(n.load() == per_producer * producers);
    }
    // Burst/drain: fully park the pool between bursts so the producer 0->1 wake restarts it.
    {
        ts::Scheduler s{ { .idle_policy = policy } };
        std::atomic<int> n{ 0 };
        constexpr int bursts = 200, per = 300;
        for (int b = 0; b < bursts; ++b)
        {
            for (int i = 0; i < per; ++i) s.submit(inc, &n);
            while (n.load(std::memory_order_acquire) != (b + 1) * per)
                std::this_thread::yield();
        }
        assert(n.load() == bursts * per);
    }
}

// parallel_for: raw helpers racing on `next`/`done`, the refcounted state lifetime (late
// queued helpers touch it after the caller returns), and nested parallel_for (caller
// participation must drain without the queued helpers running). Covers guided/unbalanced
// balance and the async variant.
void stress_parallel_for()
{
    for (int i = 0; i < 400; ++i)
    {
        constexpr int n = 2000;
        std::atomic<long long> total{ 0 };
        ts::parallel_for(n, [&total](int k) { total.fetch_add(k, std::memory_order_relaxed); },
            { .balance = ts::Balance::guided });
        assert(total.load() == static_cast<long long>(n) * (n - 1) / 2);

        total.store(0);
        ts::async_parallel_for(n, [&total](int k) { total.fetch_add(k, std::memory_order_relaxed); },
            { .balance = ts::Balance::unbalanced }).sync();
        assert(total.load() == static_cast<long long>(n) * (n - 1) / 2);
    }

    // Nested: outer occupies workers, each body runs an inner parallel_for.
    for (int i = 0; i < 200; ++i)
    {
        constexpr int outer = 32, inner = 32;
        std::atomic<long long> total{ 0 };
        ts::parallel_for(outer, [&total](int)
        {
            ts::parallel_for(inner, [&total](int) { total.fetch_add(1, std::memory_order_relaxed); });
        });
        assert(total.load() == static_cast<long long>(outer) * inner);
    }
}

#if defined(__cpp_impl_coroutine)
// Coroutine spike: the awaiter's resume fires on the thread that settles the awaited task
// (cross-thread), racing `await_suspend`'s handshake (`state_`). Many external threads each
// drive a coroutine that `co_await`s a chain of launched tasks; `sync()` joins. Exercises the
// synchronous-vs-async resume handshake + the decoupled-block lifetime under contention.
ts::Task<int> co_chain(int depth)
{
    int acc = 0;
    for (int i = 0; i < depth; ++i)
        acc = co_await ts::launch([acc] { return acc + 1; });
    co_return acc;
}

void stress_coroutine()
{
    constexpr int threads = 8, per = 400, depth = 6;
    std::atomic<int> bad{ 0 };
    {
        std::vector<std::jthread> drivers;
        for (int t = 0; t < threads; ++t)
            drivers.emplace_back([&]
            {
                for (int i = 0; i < per; ++i)
                    if (co_chain(depth).sync() != depth)
                        bad.fetch_add(1, std::memory_order_relaxed);
            });
    }   // join drivers
    assert(bad.load() == 0);
}

// Coroutine async-lock guard under contention: many threads each drive a coroutine that
// repeatedly `co_await ts::read_write(w)` the SAME object. A contended acquire defers -> the coroutine
// suspends -> resumes on the releasing thread, so this races `pipe_acquire`'s `on_acquired`
// handshake + the cross-thread resume against `pipe_release`. The pipe serializes writers, so
// the total must be exact.
ts::Task<void> co_guard_bump(ts::Guarded<int>& w, int times)
{
    for (int i = 0; i < times; ++i)
    {
        auto g = co_await ts::read_write(w);
        ++*g;
    }   // guard released each iteration
}

void stress_coroutine_guard()
{
    constexpr int threads = 8, each = 300;
    ts::Guarded<int> w{ 0 };
    {
        std::vector<std::jthread> drivers;
        for (int t = 0; t < threads; ++t)
            drivers.emplace_back([&] { co_guard_bump(w, each).sync(); });
    }   // join
    int final = w.async([](const int& v) { return v; }).sync();
    assert(final == threads * each);
}

// Deep coroutine cascade -- proves the bounded resume trampoline. A `depth`-deep chain of
// coroutines each `co_await`s the previous; all are suspended on one `Signal`. Triggering it
// resumes the innermost, whose completion resumes the next, ... -- a cascade that WITHOUT the
// `schedule_resume` trampoline would recurse (settle -> resume -> complete -> settle -> ...) one
// stack frame per level and overflow at this depth. With it, the cascade runs iteratively. Also
// checks the result threads correctly through all `depth` awaits.
ts::Task<int> co_gate(ts::Signal& gate)
{
    co_await gate;
    co_return 0;
}
ts::Task<int> co_add_prev(ts::Task<int> prev)
{
    co_return 1 + co_await std::move(prev);
}
void stress_coroutine_deep()
{
    constexpr int depth = 50000;
    ts::Signal gate;
    ts::Task<int> t = co_gate(gate);
    for (int i = 0; i < depth; ++i)
        t = co_add_prev(std::move(t));
    gate.trigger();   // cascade of `depth` resumes -- must not overflow the stack
    assert(t.sync() == depth);
}
#endif

// Deferred: per-thread recorders staging concurrently with fire-and-forget commits
// and pipe readers. Stresses the slot mutex (stage vs cut), the registration path,
// and the cut-at-execution semantics (every command lands in exactly one commit).
void stress_deferred()
{
    constexpr int threads = 6, per = 500, rounds = 20;
    for (int r = 0; r < rounds; ++r)
    {
        ts::Guarded<int> target{ 0 };
        ts::Deferred<int> d{ target };
        {
            std::vector<std::jthread> ps;
            for (int t = 0; t < threads; ++t)
                ps.emplace_back([rec = d.recorder(), &d, &target]() mutable
                {
                    for (int k = 0; k < per; ++k)
                    {
                        rec.stage([](int& v) { ++v; });
                        if ((k & 63) == 0)
                            d.commit_async();   // commits race staging (cut at execution)
                        if ((k & 31) == 0)
                            target.async([](const int& v) { (void)v; });   // readers race commits
                    }
                });
        }   // join stagers
        d.commit_async().sync();
        int final = target.async([](const int& v) { return v; }).sync();
        assert(final == threads * per);
    }

    // Parallel_recorder: per-worker placement under parallel_for (workers hit their
    // own slots, the participating caller hits the overflow lane), racing
    // fire-and-forget commits.
    {
        ts::Guarded<int> target{ 0 };
        ts::Deferred<int> d{ target };
        auto rec = d.parallel_recorder();
        constexpr int rounds_pr = 10, per_pr = 20000;
        for (int r = 0; r < rounds_pr; ++r)
        {
            ts::parallel_for(per_pr, [&rec](int) { rec.stage([](int& v) { ++v; }); });
            d.commit_async();
        }
        d.commit_async().sync();
        assert(target.async([](const int& v) { return v; }).sync() == rounds_pr * per_pr);
    }
}

// Versioned: concurrent staging + chained dynamic publishes + readers hammering the
// front. Stresses the publish chain handoff (seq_mutex_), phase-1 apply racing
// readers of the current version, the swap, and the resync READ job overlapping
// readers of the new version (with the divergence hash reading both replicas).
void stress_versioned()
{
    constexpr int stagers = 4, readers = 3, per = 400, publishes = 200;
    ts::Versioned<int> v;
    v.set_divergence_check([](const int& x) { return static_cast<std::size_t>(x); });

    std::atomic<bool> stop{ false };
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < stagers; ++t)
            threads.emplace_back([rec = v.recorder()]() mutable
            {
                for (int k = 0; k < per; ++k)
                    rec.stage([](int& x) { ++x; });
            });
        for (int t = 0; t < readers; ++t)
            threads.emplace_back([&v, &stop]
            {
                int last = 0;
                while (!stop.load(std::memory_order_acquire))
                {
                    int x = v.read([](const int& val) { return val; }).sync();
                    assert(x >= last);   // versions are monotonic
                    last = x;
                }
            });
        for (int p = 0; p < publishes; ++p)
            v.publish().sync();
        stop.store(true, std::memory_order_release);
    }   // join
    v.publish().sync();   // catch stragglers
    v.publish().sync();   // empty publish = resync fence
    assert(v.read([](const int& x) { return x; }).sync() == stagers * per);
}

// The physics sample: sealed Guarded machine + Deferred inputs + Versioned poses,
// graph flip via publish_into (phases 1-2 under the node grant, resync as a pipe
// read job admitted at node release). Also re-checks run-to-run determinism.
void stress_physics()
{
    std::size_t a = sample::physics_pose_hash(30);
    std::size_t b = sample::physics_pose_hash(30);
    assert(a == b);
    (void)a; (void)b;
}

#if defined(TS_REUSE_FORENSICS)
// Isolated forensic driver for the stress_reuse `dep.sync() == i` flake (see the forensics
// ring in task.h). Runs ONLY the reuse+prereq+retraction loop, many times per process; on a
// mismatch records everything and CONTINUES (no abort) so one long run collects many
// captures. Enabled by env TS_REUSE_ONLY=1; outer iteration count via TS_REUSE_ITERS.
const char* forensic_event_name(std::uint32_t id)
{
    using namespace ts::detail::forensics;
    switch (id)
    {
    case E_round: return "ROUND      ";
    case E_reset: return "RESET      ";
    case E_release: return "RELEASE    ";
    case E_submit: return "SUBMIT     ";
    case E_pop: return "POP        ";
    case E_claim_ok: return "CLAIM_OK   ";
    case E_claim_fail: return "CLAIM_FAIL ";
    case E_cancel_branch: return "CANCEL_BR  ";
    case E_body: return "BODY       ";
    case E_prereq_body: return "PREREQ_BODY";
    case E_settle: return "SETTLE     ";
    case E_retract_exec: return "RETRACT_EXE";
    case E_sync_ret: return "SYNC_RET   ";
    default: return "?          ";
    }
}

void forensic_dump(std::uint32_t tail)
{
    using namespace ts::detail::forensics;
    std::uint32_t end = ring_idx.load(std::memory_order_relaxed);
    std::uint32_t begin = end > tail ? end - tail : 0;
    std::printf("--- ring dump [%u..%u) ---\n", begin, end);
    for (std::uint32_t s = begin; s < end; ++s)
    {
        const Entry& e = ring[s & (ring_size - 1)];
        std::printf("  %8u %s a=%llu b=%llu tid=%04x\n", s, forensic_event_name(e.id.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(e.a.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(e.b.load(std::memory_order_relaxed)),
                    e.tid.load(std::memory_order_relaxed));
    }
    std::printf("--- end dump ---\n");
}

int reuse_forensics_main()
{
    using namespace ts::detail::forensics;
    int outer = 50;
    if (const char* it = std::getenv("TS_REUSE_ITERS"))
        outer = std::atoi(it);
    int captures = 0;

    for (int iter = 0; iter < outer; ++iter)
    {
        std::atomic<int> log{ 0 };
        auto dep = ts::task([&log]
        {
            int v = log.load(std::memory_order_relaxed);
            if (auto* w = watched.load(std::memory_order_relaxed))
                rec(E_body, static_cast<std::uint64_t>(v), w->run_state.load(std::memory_order_relaxed));
            return v;
        });
        watched.store(nullptr, std::memory_order_relaxed);   // re-arm per outer iter (fresh block)

        for (int i = 1; i <= 3000; ++i)
        {
            if (i > 1)
                dep.reset();
            TS_FORENSIC_G(E_round, static_cast<std::uint64_t>(i), static_cast<std::uint64_t>(iter));
            ts::Task<void> prereq = ts::launch([&log, i]
            {
                log.store(i, std::memory_order_relaxed);
                TS_FORENSIC_G(E_prereq_body, static_cast<std::uint64_t>(i), 0);
            });
            ts::Task<int> h = dep.after(prereq).launch();
            if (i == 1)
                watched.store(ts::detail::core_of(h).get(), std::memory_order_relaxed);
            int got = dep.sync();
            TS_FORENSIC_G(E_sync_ret, static_cast<std::uint64_t>(got), static_cast<std::uint64_t>(i));
            if (got != i)
            {
                ++captures;
                auto* c = ts::detail::core_of(h).get();
                bool completed_now, cancelled_now;
                {
                    std::scoped_lock lk(c->mutex);   // race-free capture read of the plain bools
                    completed_now = c->completed;
                    cancelled_now = c->cancelled;
                }
                std::printf("CAPTURE iter=%d round=%d got=%d run_state=%llu dispatch_arg=%llu num_locks=%u "
                            "completed=%d cancelled=%d ready=%d\n",
                            iter, i, got,
                            static_cast<unsigned long long>(c->run_state.load(std::memory_order_relaxed)),
                            static_cast<unsigned long long>(c->dispatch_arg.load(std::memory_order_relaxed)),
                            c->num_locks.load(std::memory_order_relaxed),
                            completed_now ? 1 : 0, cancelled_now ? 1 : 0,
                            c->ready.load(std::memory_order_relaxed) ? 1 : 0);
                forensic_dump(160);
                std::fflush(stdout);
            }
        }
        if ((iter + 1) % 10 == 0)
            std::printf("heartbeat: iter %d/%d, captures %d\n", iter + 1, outer, captures);
    }
    std::printf("reuse forensics done: %d outer iters, %d captures\n", outer, captures);
    return captures > 0 ? 42 : 0;
}
#endif // TS_REUSE_FORENSICS

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: last stage is visible if it hangs
#if defined(TS_REUSE_FORENSICS)
    if (std::getenv("TS_REUSE_ONLY"))
        return reuse_forensics_main();
#endif
    std::puts("tsan: scheduler stress");   stress_scheduler();
    std::puts("tsan: thread_safe stress");  stress_thread_safe();
    std::puts("tsan: inline async stress");  stress_inline_async();
    std::puts("tsan: signal stress");       stress_signal();
    std::puts("tsan: when_all stress");      stress_when_all();
    std::puts("tsan: when_all cancel stress"); stress_when_all_cancel();
    std::puts("tsan: launch stress");        stress_launch();
    std::puts("tsan: prereq stress");        stress_prereq();
    std::puts("tsan: nested stress");        stress_nested();
    std::puts("tsan: reuse stress");         stress_reuse();
    std::puts("tsan: retraction stress");    stress_retraction();
    std::puts("tsan: inline stress");        stress_inline();
    std::puts("tsan: cancel stress");        stress_cancel();
    std::puts("tsan: token body stress");    stress_token_body();
    std::puts("tsan: cancel callback stress"); stress_cancel_callback();
    std::puts("tsan: graph stress");        stress_graph();
    std::puts("tsan: graph+async stress");  stress_graph_async();
    std::puts("tsan: graph inline stress");  stress_graph_inline();
    std::puts("tsan: multi async stress");   stress_multi_async();
    std::puts("tsan: spin_then_block stress"); stress_idle_policy(ts::Idle_policy::spin_then_block);
    std::puts("tsan: handoff stress");        stress_idle_policy(ts::Idle_policy::handoff);
    std::puts("tsan: parallel_for stress");  stress_parallel_for();
#if defined(__cpp_impl_coroutine)
    std::puts("tsan: coroutine stress");     stress_coroutine();
    std::puts("tsan: coroutine guard stress"); stress_coroutine_guard();
    std::puts("tsan: coroutine deep cascade"); stress_coroutine_deep();
#endif
    std::puts("tsan: graph nested stress");  stress_graph_nested();
    std::puts("tsan: deferred stress");      stress_deferred();
    std::puts("tsan: versioned stress");     stress_versioned();
    std::puts("tsan: physics frames");       stress_physics();
    std::puts("tsan: blackboard frames");    sample::run_blackboard_sample();
    std::puts("tsan: game_frame frames");
    for (int i = 0; i < 20; ++i)
    {
        double avg = 0.0, serial = 0.0;
        float xf = 0.0f;
        sample::game_frame_stats(20, 0.2f, avg, serial, xf);
    }
    std::puts("tsan: game_frame optimised frames");
    sample::stress_game_frame_optimised(40, 4);   // gameplay Versioned + Deferred staging
    std::puts("tsan: done (no races)");
    return 0;
}
