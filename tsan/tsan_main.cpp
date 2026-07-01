// ThreadSanitizer stress driver. Exercises the concurrency paths (scheduler,
// Thread_safe reader/writer pipe, Static_task_graph + parallel_for, then/when_all)
// without the Windows-specific test harness, so it builds under clang/libstdc++
// with -fsanitize=thread on Linux/macOS. See tsan/run.sh and tsan/README.md.
//
// Run after every major change: a clean exit means TSan found no data races in
// these workloads; a race prints a report and (with halt_on_error) exits nonzero.

#include "engine.h"          // sample::run_frames
#include "scheduler.h"
#include "static_task_graph.h"
#include "thread_safe.h"

#include <atomic>
#include <cassert>
#include <cstdio>
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
    Scheduler s;
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
    ts::Thread_safe<int> obj{ 0 };
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
    int final = obj.async([](const int& v) { return v; }).get();   // FIFO drain
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
        sig.then([&fired] { fired.fetch_add(1, std::memory_order_relaxed); });

        {
            std::vector<std::jthread> threads;
            for (int t = 0; t < triggerers; ++t)
                threads.emplace_back([&] { sig.trigger(); });
            threads.emplace_back([&] { sig.get(); });   // a concurrent waiter
        }   // join
        sig.get();
        assert(fired.load() == 1);
    }
}

// Graph with internal parallel bands, re-executed in a loop.
void stress_graph()
{
    ts::Thread_safe<int> a{ 0 }, b{ 0 }, c{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& x) { x = 1; }, a);
    g.add_node([](const int& x, int& y) { y = x * 10; }, a, b);
    g.add_node([](const int& x, const int& y, int& z) { z = x + y; }, a, b, c);
    g.compile();
    for (int i = 0; i < 200; ++i)
        g.execute().get();
}

// when_all joining prerequisites that complete on different worker threads: mixed
// void + non-void + move-only, consumed apply-style. Exercises the join's remaining
// counter, slot moves, and finish across threads.
void stress_when_all()
{
    ts::Thread_safe<int> a{ 3 }, b{ 4 }, c{ 5 };
    std::atomic<int> total{ 0 };
    for (int i = 0; i < 3000; ++i)
    {
        ts::Task<void> v = a.async([](int& x) { ++x; });                          // void
        ts::Task<int> r = b.async([](const int& x) { return x; });               // value
        ts::Task<std::unique_ptr<int>> m = c.async([](const int& x) { return std::make_unique<int>(x); }); // move-only

        int s = ts::when_all(v, r, m)
            .then([](int rv, std::unique_ptr<int>& mv) { return rv + *mv; })      // apply-style, void dropped
            .get();
        total.fetch_add(s, std::memory_order_relaxed);
    }
    assert(total.load() == 3000 * 9);
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
                    int v = ts::launch([k] { return k; }).then([](int x) { return x + 1; }).get();
                    sum.fetch_add(v, std::memory_order_relaxed);
                }
            });
    }   // join
    (void)sum;
}

// Cancellation racing execution: request_cancel (a store) concurrent with the body's
// token check (a load) and with then-propagation; the block must settle exactly once.
void stress_cancel()
{
    ts::Thread_safe<int> d{ 0 };

    for (int i = 0; i < 2000; ++i)
    {
        ts::Cancellation_source src;
        ts::Task<int> t = d.async([](const int& v) { return v; }, src.token());
        ts::Task<int> u = t.then([](int v) { return v + 1; });   // propagates settle either way

        std::jthread canceller([&] { src.request_cancel(); });   // race the body
        canceller.join();

        while (!u.is_done())
            std::this_thread::yield();
        assert(u.is_done());   // settled exactly once, whether completed or cancelled
    }

    // Graph cancellation racing the run.
    ts::Thread_safe<int> a{ 0 }, b{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { ++v; }, a);
    g.add_node([](const int& x, int& y) { y = x; }, a, b);
    g.compile();
    for (int i = 0; i < 500; ++i)
    {
        ts::Cancellation_source src;
        std::jthread canceller([&] { src.request_cancel(); });
        g.execute(ts::default_scheduler(), src.token()).get();
        canceller.join();
    }
}

// A graph node accessing an object directly while other threads fire async on the
// SAME object: the per-run pipe reservation must keep them from overlapping.
void stress_graph_async()
{
    ts::Thread_safe<int> x{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { ++v; }, x);
    g.compile();

    std::atomic<bool> stop{ false };
    {
        std::vector<std::jthread> firers;
        for (int t = 0; t < 4; ++t)
            firers.emplace_back([&]
            {
                while (!stop.load(std::memory_order_relaxed))
                    x.async([](int& v) { ++v; }).get();
            });

        for (int i = 0; i < 300; ++i)
            g.execute().get();

        stop.store(true, std::memory_order_relaxed);
    }   // join firers
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: last stage is visible if it hangs
    std::puts("tsan: scheduler stress");   stress_scheduler();
    std::puts("tsan: thread_safe stress");  stress_thread_safe();
    std::puts("tsan: signal stress");       stress_signal();
    std::puts("tsan: when_all stress");      stress_when_all();
    std::puts("tsan: launch stress");        stress_launch();
    std::puts("tsan: cancel stress");        stress_cancel();
    std::puts("tsan: graph stress");        stress_graph();
    std::puts("tsan: graph+async stress");  stress_graph_async();
    std::puts("tsan: engine frames");       for (int i = 0; i < 20; ++i) sample::run_frames(20, 0.2f);
    std::puts("tsan: done (no races)");
    return 0;
}
