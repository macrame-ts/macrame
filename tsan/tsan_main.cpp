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

} // namespace

int main()
{
    std::puts("tsan: scheduler stress");   stress_scheduler();
    std::puts("tsan: thread_safe stress");  stress_thread_safe();
    std::puts("tsan: graph stress");        stress_graph();
    std::puts("tsan: engine frames");       for (int i = 0; i < 20; ++i) sample::run_frames(20, 0.2f);
    std::puts("tsan: done (no races)");
    return 0;
}
