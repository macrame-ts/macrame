// ThreadSanitizer stress driver. Exercises the concurrency paths (scheduler,
// Guarded reader/writer pipe, Static_task_graph + parallel_for, coroutine composition)
// without the Windows-specific test harness, so it builds under clang/libstdc++
// with -fsanitize=thread on Linux/macOS. See tsan/run.sh and tsan/README.md.
//
// Run after every major change: a clean exit means TSan found no data races in
// these workloads; a race prints a report and (with halt_on_error) exits nonzero.

#include "scheduler_scope.h"
#include <cstddef>

// Single-file samples (no headers) - see sample/game_frame.cpp,
// sample/physics.cpp, sample/blackboard.cpp.
namespace sample
{
void game_frame_free_stats(int frames, float time_scale,
                           double& avg_ms, double& serial_ms, float& transform0);
void game_frame_stats(int frames, float time_scale,
                      double& avg_ms, double& serial_ms, float& transform0);
void stress_game_frame_optimised(int frames, int workers);
void run_blackboard_sample();
void stress_coloring(int frames);
std::size_t physics_pose_hash(int frames);   // final snapshot hash after `frames` frames
}
#include "ts/parallel_for.h"
#include "ts/scheduler.h"
#include "ts/static_task_graph.h"
#include "ts/guarded.h"
#include "ts/deferred.h"
#include "ts/versioned.h"
#include "graph_trace.h"   // tools/: worker-side stamps + settle-side fold under TSan
#include "test_util.h"     // tests/: the Rw_probe dual oracle (shared with the unit suite)

#include "ts/coroutine_support.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <deque>
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
    ts::Scheduler_scope scope{ {} };
    ts::Scheduler& s = ts::global_scheduler();
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
    ts::Guarded<int> obj{ ts::Named{}, 0 };
    std::atomic<int> reads{ 0 };
    {
        std::vector<std::jthread> producers;
        for (int t = 0; t < threads; ++t)
        {
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
        }
    }   // join producers
    [[maybe_unused]] int final = obj.async([](const int& v) { return v; }).sync();   // FIFO drain
    assert(final == threads * per / 2);
}

// Concurrent access on one object mixing `access` (inline-when-free) and `async` (always
// enqueued) jobs across threads. An `access` job runs on the calling thread when the pipe is
// momentarily free, else defers to the queue; either way every increment must land exactly once
// and the pipe must drain. Stresses pipe_try_inline's acquire/run/release racing pipe_enqueue.
void stress_inline_async()
{
    constexpr int threads = 8, per = 2000;
    ts::Guarded<int> obj{ ts::Named{}, 0 };
    std::atomic<int> reads{ 0 };
    {
        std::vector<std::jthread> producers;
        for (int t = 0; t < threads; ++t)
        {
            producers.emplace_back([&obj, &reads]
            {
                for (int k = 0; k < per; ++k)
                {
                    bool opportunistic = (k % 3) == 0;   // some inline-when-free, some always enqueued
                    if (k & 1)
                    {
                        auto w = [](int& v) { ++v; };
                        if (opportunistic) obj.access(w).sync(); else obj.async(w);
                    }
                    else
                    {
                        auto r = [&reads](const int& v)
                            { reads.fetch_add(1, std::memory_order_relaxed); return v; };
                        if (opportunistic) (void)obj.access(r).sync(); else obj.async(r);
                    }
                }
            });
        }
    }   // join producers
    [[maybe_unused]] int final = obj.async([](const int& v) { return v; }).sync();   // FIFO drain
    assert(final == threads * per / 2);
}

// The pipe-rebase reader/writer stress: N threads hammer one object with a randomized
// read/write x async/access mix. `Rw_probe`'s unsynchronized payload is the TSan detector
// - any reader/writer overlap the pipe fails to prevent shows up as a race on it (the
// probe's own atomics are synchronized, so they alone would hide it).
void pipe_rw_producers(ts::Guarded<tests::Rw_probe>& probe, int threads, int per)
{
    std::vector<std::jthread> producers;
    for (int t = 0; t < threads; ++t)
    {
        producers.emplace_back([&probe, t, per]
        {
            for (int k = 0; k < per; ++k)
            {
                std::uint32_t seed = static_cast<std::uint32_t>(t * 100003 + k);
                bool write = (seed % 3) == 0;   // ~1/3 writes
                bool inl = (seed % 2) == 0;     // half via access (may run inline)
                if (write && inl)
                    probe.access([seed](tests::Rw_probe& p) { p.observe_write(seed); }).sync();
                else if (write)
                    probe.async([seed](tests::Rw_probe& p) { p.observe_write(seed); });
                else if (inl)
                    probe.access([seed](const tests::Rw_probe& p) { p.observe_read(seed); }).sync();
                else
                    probe.async([seed](const tests::Rw_probe& p) { p.observe_read(seed); });
            }
        });
    }
}   // join

void stress_pipe_rw()
{
    ts::Guarded<tests::Rw_probe> probe{ ts::Named{} };
    pipe_rw_producers(probe, 8, 3000);
    [[maybe_unused]] bool bad = probe.async([](const tests::Rw_probe& p) { return p.violated(); }).sync();
    assert(!bad);
}

// Same, worker-less: bodies run inline at submit, cross-thread pipe work migrates to the
// releasing thread; the pipe still serializes. Exercises R7 (no mutex; inline-at-submit).
void stress_pipe_rw_worker_less()
{
    ts::Scheduler_scope scope{ ts::Scheduler_config{ .single_threaded = true } };
    ts::Guarded<tests::Rw_probe> probe{ ts::Named{} };
    pipe_rw_producers(probe, 4, 1500);
    [[maybe_unused]] bool bad = probe.async([](const tests::Rw_probe& p) { return p.violated(); }).sync();
    assert(!bad);
}

// Lifetime churn targeting the `wait_until_idle` drain: create/destroy `Guarded`s with
// in-flight work from several threads, dropping every task handle so the destructor's drain
// races the last job's completion + idle-notify, and blocks can be freed by a completing
// predecessor mid-push (the push-UAF bracket). A reader/writer mix so the notify fires from
// both release paths (a writer draining to empty, a last reader hitting
// `active_readers == 0`), and the fast stack reuse across iterations gives TSan a hot
// address to flag if a completing job touches the pipe after the drain releases the
// destroyer.
void stress_pipe_lifetime()
{
    constexpr int threads = 4, iters = 2000, jobs = 12;
    std::vector<std::jthread> ths;
    for (int t = 0; t < threads; ++t)
    {
        ths.emplace_back([]
        {
            for (int i = 0; i < iters; ++i)
            {
                ts::Guarded<int> obj{ ts::Named{}, 0 };
                for (int k = 0; k < jobs; ++k)
                {
                    if (k & 1)
                        obj.async([](int& v) { ++v; });                 // writer
                    else
                        obj.async([](const int& v) { return v; });      // reader
                }
            }   // handles all dropped; ~Guarded drains, racing the last completion/notify
        });
    }
}   // join

// The synced variant of the same drain, which is the sharper ordering: a task settles as
// settle -> notify waiters -> `on_complete` -> pipe release, so a returned `sync()` says
// nothing about the grant - the release is still ahead of the settling thread. Destroying
// the object at exactly that point leaves the drain to catch a release the destroyer has
// already been woken past. Heap objects (not stack) so the freed pipe is a distinct address
// TSan/ASan can attribute; both the reader-last and writer-last release paths, plus a
// multi-object task whose links are released one at a time while the first object's
// destructor may already be running.
void stress_pipe_sync_then_destroy()
{
    constexpr int threads = 4, iters = 1500;
    std::vector<std::jthread> ths;
    for (int t = 0; t < threads; ++t)
    {
        ths.emplace_back([]
        {
            for (int i = 0; i < iters; ++i)
            {
                auto* obj = new ts::Guarded<int>{ ts::Named{}, 0 };
                obj->async([](int& v) { ++v; });   // queued ahead of the sync target
                if (i & 1)
                    obj->async([](int& v) { v *= 10; return v; }).sync();
                else
                    obj->async([](const int& v) { return v; }).sync();
                delete obj;

                auto* first = new ts::Guarded<int>{ ts::Named{}, 1 };
                auto* second = new ts::Guarded<int>{ ts::Named{}, 2 };
                ts::async([](int& x, const int& y) { x += y; return x; }, *first, *second).sync();
                delete first;
                delete second;
            }
        });
    }
}   // join

// Reservation coexistence: a graph run holding shared objects per-node while async hammers
// the same objects - the per-node acquire must exclude a writer node from any async and
// let a reader node overlap an async read (the docs/internals/task-internals.md §10 race). TSan on
// the payload; the oracle also asserts the invariant.
void stress_pipe_reservation()
{
    ts::Guarded<tests::Rw_probe> a{ ts::Named{ "a" } }, b{ ts::Named{ "b" } };
    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](tests::Rw_probe& p) { p.observe_write(1); }, a);        // writer on a
    g.add_node(ts::Named{}, [](const tests::Rw_probe& p) { p.observe_read(2); }, a);   // reader on a (ordered after)
    g.add_node(ts::Named{}, [](tests::Rw_probe& p) { p.observe_write(3); }, b);        // writer on b
    g.compile();

    std::atomic<bool> stop{ false };
    std::vector<std::jthread> asyncs;
    for (int t = 0; t < 3; ++t)
    {
        asyncs.emplace_back([&]
        {
            std::uint32_t s = 0;
            // Bounded in-flight window (mirrors the pipe_tests hammer): a wedged pipe must
            // saturate at `window` retained entries, not balloon the process/WSL VM to OOM
            // while the run.sh timeout counts down.
            constexpr std::size_t window = 64;
            std::deque<ts::Task<void>> inflight;
            while (!stop.load(std::memory_order_relaxed))
            {
                while (inflight.size() >= window && !stop.load(std::memory_order_relaxed))
                {
                    if (inflight.front().is_done())
                        inflight.pop_front();
                    else
                        std::this_thread::yield();
                }
                if (inflight.size() >= window)
                    break;   // stopped while saturated
                inflight.push_back(a.async([s](const tests::Rw_probe& p) { p.observe_read(s); }));
                inflight.push_back(b.async([s](tests::Rw_probe& p) { p.observe_write(s); }));
                ++s;
            }
        });
    }
    for (int r = 0; r < 200; ++r)
        g.execute().sync();
    stop.store(true, std::memory_order_relaxed);
    asyncs.clear();   // join

    [[maybe_unused]] bool bad = a.async([](const tests::Rw_probe& p) { return p.violated(); }).sync()
             || b.async([](const tests::Rw_probe& p) { return p.violated(); }).sync();
    assert(!bad);
}

// Many threads triggering one Signal concurrently while others wait / attach
// continuations: idempotent `complete()` must fire exactly once with no race.
ts::Task<void> observe_signal(ts::Signal sig, std::atomic<int>& fired)
{
    co_await sig;
    fired.fetch_add(1, std::memory_order_relaxed);
}

void stress_signal()
{
    constexpr int rounds = 2000, triggerers = 6;
    for (int r = 0; r < rounds; ++r)
    {
        ts::Signal sig;
        std::atomic<int> fired{ 0 };
        // An awaiting observer, joined on its handle: `sig.sync()` waits for the signal to
        // settle, not for work the settle resumes, so asserting `fired` right after
        // `sig.sync()` would be an over-assertion.
        ts::Task<void> fired_done = observe_signal(sig, fired);

        {
            std::vector<std::jthread> threads;
            for (int t = 0; t < triggerers; ++t)
                threads.emplace_back([&] { sig.trigger(); });
            threads.emplace_back([&] { sig.sync(); });   // a concurrent waiter
        }   // join
        sig.sync();
        fired_done.sync();   // the observer has now run
        assert(fired.load() == 1);
    }

    // A resettable Signal with a LIVE waiter overlapping reset() - the shape that exposed the
    // reset()-vs-wait() data race on `completed` (review finding T2). reset() now takes the
    // block mutex, so this must be TSan-clean; it is the regression guard for that fix. The
    // prior version joined every waiter before reset(), so reset() never overlapped a live
    // wait() - which is exactly why the race went unobserved.
    {
        ts::Signal sig;
        std::atomic<bool> stop{ false };
        std::jthread waiter([&] { while (!stop.load(std::memory_order_relaxed)) sig.sync(); });
        for (int r = 0; r < 50000; ++r)
        {
            sig.trigger();   // completed = true, under the block mutex
            sig.reset();     // completed = false, now under the mutex too - no race with wait()
        }
        stop.store(true, std::memory_order_relaxed);
        sig.trigger();       // let the waiter observe `stop` and exit its loop before the join
    }
}

// Graph with internal parallel bands, re-executed in a loop - with an aggregating trace
// attached, so the worker-side stamps and the settle-side fold run under TSan (stamps on
// several workers, fold on the settling one, stats read on the main thread after sync).
void stress_graph()
{
    ts::Guarded<int> a{ ts::Named{}, 0 }, b{ ts::Named{}, 0 }, c{ ts::Named{}, 0 };
    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](int& x) { x = 1; }, a);
    g.add_node(ts::Named{}, [](const int& x, int& y) { y = x * 10; }, a, b);
    g.add_node(ts::Named{}, [](const int& x, const int& y, int& z) { z = x + y; }, a, b, c);
    g.compile();
    ts::tools::Graph_trace trace;
    g.set_trace(&trace);
    for (int i = 0; i < 200; ++i)
        g.execute().sync();
    assert(trace.run_count() == 200);
    assert(trace.node_stats(0).runs == 200);
    g.set_trace(nullptr);
}

// A graph node fans out parallel_for sub-work over disjoint elements of the object it owns,
// re-executed in a loop with a conflicting reader successor. Stresses the node-as-block
// path: current_task set/restore on a graph node, the synchronous join gating the node's
// completion, the node_complete continuation gating the run and the successor, and
// inherited-access-scope reads/writes on the shared array from the helpers.
void stress_graph_nested()
{
    constexpr int n = 32;
    ts::Guarded<std::array<int, n>> arr{ ts::Named{} };
    std::atomic<int> sum{ 0 };

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](std::array<int, n>& a)
    {
        ts::parallel_for(n, [&a](int k) { a[k] = k; });
    }, arr);
    g.add_node(ts::Named{}, [&sum](const std::array<int, n>& a)
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

// Nested graph runs under the lend protocol (docs/internals/coroutine-first.md §4.8): an outer node
// holds write grants and awaits an inner graph over the same objects, so every inner node
// runs on the lent grant with no pipe turn of its own. External asyncs hammer the objects
// throughout - they see only the outer node's hold, since the pipe never sees the lend, so
// the `Rw_probe` oracle must observe no reader/writer overlap. The re-runs exercise the link
// slab rebinding both ways (lent subset, then the full set for the standalone inner run).
void stress_graph_nested_runs()
{
    ts::Guarded<tests::Rw_probe> a{ ts::Named{ "a" } }, b{ ts::Named{ "b" } };

    ts::Static_task_graph inner;
    inner.add_node(ts::Named{}, [](tests::Rw_probe& p) { p.observe_write(1); }, a);
    inner.add_node(ts::Named{}, [](const tests::Rw_probe& p) { p.observe_read(2); }, b);
    inner.compile();

    ts::Static_task_graph outer;
    outer.add_node(ts::Named{}, [&inner](tests::Rw_probe& pa, tests::Rw_probe& pb) -> ts::Task<void>
    {
        pa.observe_write(3);
        co_await inner.execute();          // both objects lent (write covers read)
        pb.observe_write(4);
    }, a, b);
    outer.compile();

    std::atomic<bool> stop{ false };
    std::vector<std::jthread> hammers;
    for (int t = 0; t < 3; ++t)
    {
        hammers.emplace_back([&]
        {
            constexpr std::size_t window = 64;
            std::deque<ts::Task<void>> inflight;
            while (!stop.load(std::memory_order_relaxed))
            {
                while (inflight.size() >= window && !stop.load(std::memory_order_relaxed))
                {
                    if (inflight.front().is_done())
                        inflight.pop_front();
                    else
                        std::this_thread::yield();
                }
                if (inflight.size() >= window)
                    break;
                inflight.push_back(a.async([](const tests::Rw_probe& p) { p.observe_read(5); }));
                inflight.push_back(b.async([](tests::Rw_probe& p) { p.observe_write(6); }));
            }
        });
    }

    for (int i = 0; i < 200; ++i)
    {
        outer.execute().sync();
        inner.execute().sync();   // same graph, standalone: full link binding restored
    }

    stop.store(true, std::memory_order_relaxed);
    hammers.clear();   // join

    [[maybe_unused]] bool bad = a.async([](const tests::Rw_probe& p) { return p.violated(); }).sync()
             || b.async([](const tests::Rw_probe& p) { return p.violated(); }).sync();
    assert(!bad);
}

// Two different graphs over overlapping objects, run concurrently from separate threads,
// while asyncs hammer the same objects (task-internals §10 scenario 2). Each graph's nodes
// take their turns in canonical pipe-address order over the same address-sorted objects, and
// the pipe serializes across graphs exactly as it does between a node and an async - so
// there should be no wait cycle and no reader/writer overlap. TSan on the probe payload; the
// oracle asserts the invariant independently.
void stress_concurrent_graphs()
{
    constexpr int rounds = 400;
    ts::Guarded<tests::Rw_probe> a{ ts::Named{ "a" } }, b{ ts::Named{ "b" } }, c{ ts::Named{ "c" } };

    ts::Static_task_graph g1;
    g1.add_node(ts::Named{}, [](tests::Rw_probe& p) { p.observe_write(1); }, a);
    g1.add_node(ts::Named{}, [](const tests::Rw_probe& p, tests::Rw_probe& q) { p.observe_read(2); q.observe_write(3); }, b, c);
    g1.compile();

    ts::Static_task_graph g2;                    // opposite declaration order on the same objects
    g2.add_node(ts::Named{}, [](tests::Rw_probe& p) { p.observe_write(4); }, b);
    g2.add_node(ts::Named{}, [](tests::Rw_probe& p, const tests::Rw_probe& q) { p.observe_write(5); q.observe_read(6); }, a, b);
    g2.compile();

    std::atomic<bool> stop{ false };
    std::vector<std::jthread> hammers;
    for (int t = 0; t < 2; ++t)
    {
        hammers.emplace_back([&]
        {
            constexpr std::size_t window = 64;
            std::deque<ts::Task<void>> inflight;
            while (!stop.load(std::memory_order_relaxed))
            {
                while (inflight.size() >= window && !stop.load(std::memory_order_relaxed))
                {
                    if (inflight.front().is_done())
                        inflight.pop_front();
                    else
                        std::this_thread::yield();
                }
                if (inflight.size() >= window)
                    break;
                inflight.push_back(a.async([](const tests::Rw_probe& p) { p.observe_read(7); }));
                inflight.push_back(b.async([](tests::Rw_probe& p) { p.observe_write(8); }));
            }
        });
    }

    {
        std::jthread second([&]
        {
            for (int i = 0; i < rounds; ++i)
                g2.execute().sync();
        });
        for (int i = 0; i < rounds; ++i)
            g1.execute().sync();
    }   // join the graph drivers

    stop.store(true, std::memory_order_relaxed);
    hammers.clear();   // join

    [[maybe_unused]] bool bad = a.async([](const tests::Rw_probe& p) { return p.violated(); }).sync()
             || b.async([](const tests::Rw_probe& p) { return p.violated(); }).sync()
             || c.async([](const tests::Rw_probe& p) { return p.violated(); }).sync();
    assert(!bad);
}

ts::Task<int> launch_plus_one(int k)
{
    co_return co_await ts::launch([k] { return k; }) + 1;
}

// Many external threads launching standalone tasks (body-in-block) concurrently, each
// consumed by an awaiting coroutine and joined at the boundary.
void stress_launch()
{
    constexpr int threads = 6, per = 1500;
    std::atomic<int> sum{ 0 };
    {
        std::vector<std::jthread> ps;
        for (int t = 0; t < threads; ++t)
        {
            ps.emplace_back([&]
            {
                for (int k = 0; k < per; ++k)
                    sum.fetch_add(launch_plus_one(k).sync(), std::memory_order_relaxed);
            });
        }
    }   // join
    (void)sum;
}

// Awaited fork-join under oversubscription: outer coroutines saturate the workers and
// await their inner tasks - suspension frees the worker, so the inner tasks always find
// one (the deadlock the old blocking join needed retraction for is structurally absent).
// Stresses suspension/resume racing worker completion across two nesting levels.
ts::Task<void> awaited_inner_join(std::atomic<int>& total)
{
    ts::Task<void> a = ts::launch([&total] { total.fetch_add(1, std::memory_order_relaxed); });
    ts::Task<void> b = ts::launch([&total] { total.fetch_add(1, std::memory_order_relaxed); });
    ts::Task<void> c = ts::launch([&total] { total.fetch_add(1, std::memory_order_relaxed); });
    co_await a;
    co_await b;
    co_await c;
}

void stress_awaited_forkjoin()
{
    const int outer = 2 * static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    for (int i = 0; i < 200; ++i)
    {
        std::atomic<int> total{ 0 };
        std::vector<ts::Task<void>> tasks;
        for (int o = 0; o < outer; ++o)
            tasks.push_back(awaited_inner_join(total));
        for (auto& t : tasks)
            t.sync();
        assert(total.load() == outer * 3);
    }
}

// Cancellation racing execution: request_cancel (a store) concurrent with the body's
// token check (a load); the block must settle exactly once, completed or cancelled.
void stress_cancel()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };

    for (int i = 0; i < 2000; ++i)
    {
        ts::Cancellation_source src;
        ts::Task<int> t = d.async([](const int& v) { return v; }, { .token = src.token() });

        std::jthread canceller([&] { src.request_cancel(); });   // race the body
        canceller.join();

        while (!t.is_done())
            std::this_thread::yield();
        assert(t.is_done());   // settled exactly once, whether completed or cancelled
    }

    // Graph cancellation racing the run.
    ts::Guarded<int> a{ ts::Named{}, 0 }, b{ ts::Named{}, 0 };
    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](int& v) { ++v; }, a);
    g.add_node(ts::Named{}, [](const int& x, int& y) { y = x; }, a, b);
    g.compile();
    for (int i = 0; i < 500; ++i)
    {
        ts::Cancellation_source src;
        std::jthread canceller([&] { src.request_cancel(); });
        g.execute({.token = src.token()}).sync();
        canceller.join();
    }
}

// A token-taking body polling the token while another thread cancels: request_cancel racing
// the body's polling read of the shared cancel flag, and the block settling completed after a
// cooperative early-out.
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

        while (!started.load(std::memory_order_relaxed))
            std::this_thread::yield();
        src.request_cancel();

        assert(t.sync() == 7);            // body early-outed, completed
        assert(stage.load() == 1);
    }

    // Same, through an async accessor (body runs on the pipe): request_cancel races the
    // accessor's polling read.
    ts::Guarded<int> d{ ts::Named{}, 5 };
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
}

// Cancel callbacks racing request_cancel: several threads register a Cancel_callback that
// lives briefly then destroys, while another thread cancels. Stresses the callback-list
// mutex, the fire-immediately-when-already-requested path, and - the delicate one - the
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
            {
                threads.emplace_back([&]
                {
                    ts::Cancel_callback cb(src.token(), [&fired] { fired.fetch_add(1, std::memory_order_relaxed); });
                    std::this_thread::yield();   // let its dtor race the cancel's firing
                });
            }
            threads.emplace_back([&] { src.request_cancel(); });
        }   // join: each callback either fired or was deregistered first; neither races
        (void)fired;
    }
}

// A graph node accessing an object directly while other threads fire async on the
// same object: the per-run pipe reservation must keep them from overlapping.
// Per-node mode-aware acquire/release coexisting with out-of-band async. Multi-object
// nodes (incremental canonical-order acquire), a gap node (x free between its accessors),
// and mixed read/write nodes, while async readers and writers hammer the same objects --
// stresses pipe_acquire/pipe_release (both modes), the acquire callback chain racing async
// completion, and the gap windows. A writer node must never overlap any async on its object;
// concurrent reads may overlap (allowed).
void stress_graph_async()
{
    ts::Guarded<int> x{ ts::Named{}, 0 }, y{ ts::Named{}, 0 };
    ts::Static_task_graph g;
    ts::Graph_node n1 = g.add_node(ts::Named{}, [](int& a) { ++a; }, x);                       // write x
    ts::Graph_node n2 = g.add_node(ts::Named{}, [](int& a, int& b) { ++a; ++b; }, x, y);       // write x (handoff from n1) + write y
    ts::Graph_node n3 = g.add_node(ts::Named{}, [](int& a) { ++a; }, x);                       // write x (handoff from n2)
    n2.after(n1);
    n3.after(n2);
    g.compile();

    std::atomic<bool> stop{ false };
    {
        std::vector<std::jthread> firers;
        for (int t = 0; t < 3; ++t)
        {
            firers.emplace_back([&]
            {
                while (!stop.load(std::memory_order_relaxed))
                {
                    x.async([](int& v) { ++v; }).sync();                 // write x
                    x.async([](const int& v) { return v; }).sync();      // read x
                    y.async([](const int& v) { return v; }).sync();      // read y
                }
            });
        }

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
    ts::Guarded<int> x{ ts::Named{}, 0 };
    ts::Static_task_graph g;
    ts::Graph_node a = g.add_node(ts::Named{}, [](int& v) { ++v; }, x).set_inline();
    ts::Graph_node b = g.add_node(ts::Named{}, [](int& v) { ++v; }, x).set_inline();
    ts::Graph_node c = g.add_node(ts::Named{}, [](int& v) { ++v; }, x).set_inline();
    b.after(a);
    c.after(b);
    g.compile();

    std::atomic<bool> stop{ false };
    {
        std::vector<std::jthread> firers;
        for (int t = 0; t < 3; ++t)
        {
            firers.emplace_back([&]
            {
                while (!stop.load(std::memory_order_relaxed))
                    x.async([](int& v) { ++v; }).sync();   // contends the inline nodes' object
            });
        }

        for (int i = 0; i < 300; ++i)
            g.execute().sync();

        stop.store(true, std::memory_order_relaxed);
    }   // join firers
}

// Multi-object async under contention: concurrent multi-object asyncs over the same pair in
// opposite declared orders (canonical-order acquire must keep them deadlock-free), mixed with
// single-object asyncs on each. Stresses multi_acquire's chain (immediate + deferred), the
// release-on-completion continuation, and cross-object hold-and-wait (no cycle).
void stress_multi_async()
{
    ts::Guarded<int> a{ ts::Named{}, 0 }, b{ ts::Named{}, 0 };
    {
        std::vector<std::jthread> firers;
        for (int t = 0; t < 4; ++t)
        {
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
        }
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
        ts::Scheduler_scope scope{ { .idle_policy = policy } };
        ts::Scheduler& s = ts::global_scheduler();
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
        ts::Scheduler_scope scope{ { .idle_policy = policy } };
        ts::Scheduler& s = ts::global_scheduler();
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
        ts::parallel_for_async(n, [&total](int k) { total.fetch_add(k, std::memory_order_relaxed); },
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
        {
            drivers.emplace_back([&]
            {
                for (int i = 0; i < per; ++i)
                {
                    if (co_chain(depth).sync() != depth)
                        bad.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    }   // join drivers
    assert(bad.load() == 0);
}

// Coroutine async-lock guard under contention: many threads each drive a coroutine that
// repeatedly `co_await ts::read_write(w)` the same object. A contended acquire defers -> the coroutine
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
    ts::Guarded<int> w{ ts::Named{}, 0 };
    {
        std::vector<std::jthread> drivers;
        for (int t = 0; t < threads; ++t)
            drivers.emplace_back([&] { co_guard_bump(w, each).sync(); });
    }   // join
    [[maybe_unused]] int final = w.async([](const int& v) { return v; }).sync();
    assert(final == threads * each);
}

// Deep coroutine cascade - proves the bounded resume trampoline. A `depth`-deep chain of
// coroutines each `co_await`s the previous; all are suspended on one `Signal`. Triggering it
// resumes the innermost, whose completion resumes the next, ... - a cascade that without the
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
    gate.trigger();   // cascade of `depth` resumes - must not overflow the stack
    assert(t.sync() == depth);
}

// Deferred: per-thread recorders staging concurrently with fire-and-forget commits
// and pipe readers. Stresses the slot mutex (stage vs cut), the registration path,
// and the cut-at-execution semantics (every command lands in exactly one commit).
void stress_deferred()
{
    constexpr int threads = 6, per = 500, rounds = 20;
    for (int r = 0; r < rounds; ++r)
    {
        ts::Guarded<int> target{ ts::Named{}, 0 };
        ts::Deferred<int> d{ target };
        {
            std::vector<std::jthread> ps;
            for (int t = 0; t < threads; ++t)
            {
                ps.emplace_back([rec = d.recorder(), &d, &target]() mutable
                {
                    for (int k = 0; k < per; ++k)
                    {
                        rec.stage([](int& v) { ++v; });
                        if ((k & 63) == 0)
                            (void)d.commit();   // commits race staging (cut at execution)
                        if ((k & 31) == 0)
                            target.async([](const int& v) { (void)v; });   // readers race commits
                    }
                });
            }
        }   // join stagers
        d.commit().sync();
        [[maybe_unused]] int final = target.async([](const int& v) { return v; }).sync();
        assert(final == threads * per);
    }

    // Parallel_recorder: per-worker placement under parallel_for (workers hit their
    // own slots, the participating caller hits the overflow lane), racing
    // fire-and-forget commits.
    {
        ts::Guarded<int> target{ ts::Named{}, 0 };
        ts::Deferred<int> d{ target };
        auto rec = d.parallel_recorder();
        constexpr int rounds_pr = 10, per_pr = 20000;
        for (int r = 0; r < rounds_pr; ++r)
        {
            ts::parallel_for(per_pr, [&rec](int) { rec.stage([](int& v) { ++v; }); });
            (void)d.commit();
        }
        d.commit().sync();
        assert(target.async([](const int& v) { return v; }).sync() == rounds_pr * per_pr);
    }
}

// Versioned: concurrent staging + chained dynamic publishes + readers hammering the
// front. Stresses the publish chain handoff (seq_mutex_), phase-1 apply racing
// readers of the current version, the swap, and the resync read job overlapping
// readers of the new version (with the divergence hash reading both replicas).
void stress_versioned()
{
    constexpr int stagers = 4, readers = 3, per = 400, publishes = 200;
    ts::Versioned<int> v{ ts::Named{} };
    v.set_divergence_check([](const int& x) { return static_cast<std::size_t>(x); });

    std::atomic<bool> stop{ false };
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < stagers; ++t)
        {
            threads.emplace_back([rec = v.recorder()]() mutable
            {
                for (int k = 0; k < per; ++k)
                    rec.stage([](int& x) { ++x; });
            });
        }
        for (int t = 0; t < readers; ++t)
        {
            threads.emplace_back([&v, &stop]
            {
                [[maybe_unused]] int last = 0;
                while (!stop.load(std::memory_order_acquire))
                {
                    int x = v.read([](const int& val) { return val; }).sync();
                    assert(x >= last);   // versions are monotonic
                    last = x;
                }
            });
        }
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

} // namespace

// The entry point, renamed when this TU is compiled into the Windows binary.
//
// Everything above lives in an anonymous namespace, so `main` is the only symbol that would
// collide with `src/main.cpp` - and it is also the only thing that references those static
// functions, so dropping it outright would trade a link error for a wall of
// unused-function warnings. Renaming keeps the whole TU compiled and referenced while
// contributing no second `main`. The Windows build never calls it; it is there so an
// API-tightening change fails on the developer's own machine instead of at commit time or in
// a Linux CI job (this file used to be invisible to MSBuild, and drifted).
#ifdef TS_TSAN_NO_MAIN
namespace ts::tsan { int run_all(); }
int ts::tsan::run_all()
#else
int main()
#endif
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: last stage is visible if it hangs
    ts::create_scheduler();   // ambient scheduler for the stress stages; the Scheduler_scope ones reconfigure it
    std::puts("tsan: scheduler stress");   stress_scheduler();
    std::puts("tsan: thread_safe stress");  stress_thread_safe();
    std::puts("tsan: pipe rw stress");       stress_pipe_rw();
    std::puts("tsan: pipe rw worker-less");  stress_pipe_rw_worker_less();
    std::puts("tsan: pipe lifetime stress"); stress_pipe_lifetime();
    std::puts("tsan: sync-then-destroy stress"); stress_pipe_sync_then_destroy();
    std::puts("tsan: pipe reservation stress"); stress_pipe_reservation();
    std::puts("tsan: inline async stress");  stress_inline_async();
    std::puts("tsan: signal stress");       stress_signal();
    std::puts("tsan: launch stress");        stress_launch();
    std::puts("tsan: awaited fork-join stress"); stress_awaited_forkjoin();
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
    std::puts("tsan: coroutine stress");     stress_coroutine();
    std::puts("tsan: coroutine guard stress"); stress_coroutine_guard();
    std::puts("tsan: coroutine deep cascade"); stress_coroutine_deep();
    std::puts("tsan: graph nested stress");  stress_graph_nested();
    std::puts("tsan: nested graph runs");    stress_graph_nested_runs();
    std::puts("tsan: concurrent graphs");    stress_concurrent_graphs();
    std::puts("tsan: deferred stress");      stress_deferred();
    std::puts("tsan: versioned stress");     stress_versioned();
    std::puts("tsan: physics frames");       stress_physics();
    std::puts("tsan: blackboard frames");    sample::run_blackboard_sample();
    std::puts("tsan: coloring frames");      sample::stress_coloring(10);
    std::puts("tsan: game_frame frames");
    for (int i = 0; i < 20; ++i)
    {
        double avg = 0.0, serial = 0.0;
        float xf = 0.0f;
        sample::game_frame_stats(20, 0.2f, avg, serial, xf);
    }
    std::puts("tsan: game_frame graph-free frames");
    for (int i = 0; i < 10; ++i)
    {
        // The same frame with no graph: ~34 multi-object asyncs a chain of coroutines
        // orders by hand. Covers the coroutine composition surface the graph path does not
        // (per-frame chain frames, cross-chain handle awaits, per-frame recorder mint).
        double avg = 0.0, serial = 0.0;
        float xf = 0.0f;
        sample::game_frame_free_stats(20, 0.2f, avg, serial, xf);
    }
    std::puts("tsan: game_frame optimised frames");
    sample::stress_game_frame_optimised(40, 4);   // gameplay Versioned + Deferred staging
    std::puts("tsan: done (no races)");
    return 0;
}
