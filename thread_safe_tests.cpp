#include "thread_safe_tests.h"
#include "thread_safe.h"
#include "static_task_graph.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{

// A thread-unsafe "system": encapsulated, every public method guarded. Read/write
// is deduced from each method's const-ness, matching the context the pipe installs.
class Counter
{
public:
    void increment()
    {
        TS_CHECK_ACCESS();   // non-const -> read_write
        ++value_;
    }

    int value() const
    {
        TS_CHECK_ACCESS();   // const -> read_only
        return value_;
    }

private:
    int value_ = 0;
};

// Instrumentation shared by the reader/writer tests.
struct Probe
{
    std::atomic<int> active{ 0 };        // jobs currently running on the pipe
    std::atomic<int> max_readers{ 0 };   // peak concurrent readers observed
    std::atomic<bool> writing{ false };  // a writer is currently running
    std::atomic<bool> violated{ false }; // exclusivity broken
};

void record_max(std::atomic<int>& max, int value)
{
    int prev = max.load();
    while (value > prev && !max.compare_exchange_weak(prev, value)) {}
}

// FIFO serialization is correct: 1000 writes then a read observes the final value.
bool test_serial_correctness()
{
    ts::Thread_safe<Counter> counter;

    for (int i = 0; i < 1000; ++i)
        counter.async([](Counter& c) { c.increment(); });

    ts::Task<int> result = counter.async([](const Counter& c) { return c.value(); });

    int v = result.get();
    bool ok = (v == 1000);
    std::printf("  serial writes: counter = %d (expected 1000) -> %s\n", v, ok ? "ok" : "FAIL");
    return ok;
}

// Consecutive readers run concurrently: peak concurrency should exceed 1.
bool test_concurrent_readers()
{
    Probe probe;
    ts::Thread_safe<int> data{ 7 };
    std::vector<ts::Task<int>> tasks;

    for (int i = 0; i < 16; ++i)
        tasks.push_back(data.async([&probe](const int& v)
        {
            int now = probe.active.fetch_add(1) + 1;
            record_max(probe.max_readers, now);
            std::this_thread::sleep_for(3ms);
            probe.active.fetch_sub(1);
            return v;
        }));

    for (auto& t : tasks)
        t.get();

    int peak = probe.max_readers.load();
    bool ok = (peak > 1);
    std::printf("  concurrent readers: peak = %d (expected > 1) -> %s\n", peak, ok ? "ok" : "FAIL");
    return ok;
}

// A writer runs alone: no reader or other writer overlaps it, and writes apply in order.
bool test_writer_exclusion()
{
    Probe probe;
    ts::Thread_safe<int> data{ 0 };
    std::vector<ts::Task<void>> tasks;
    int writes = 0;

    // bursts of readers separated by a writer
    for (int round = 0; round < 20; ++round)
    {
        for (int r = 0; r < 4; ++r)
            tasks.push_back(data.async([&probe](const int& v)
            {
                probe.active.fetch_add(1);
                if (probe.writing.load())
                    probe.violated.store(true);       // a reader ran during a write
                std::this_thread::sleep_for(1ms);
                probe.active.fetch_sub(1);
                (void)v;
            }));

        tasks.push_back(data.async([&probe](int& v)
        {
            probe.writing.store(true);
            if (probe.active.fetch_add(1) + 1 != 1)
                probe.violated.store(true);           // something else ran during a write
            std::this_thread::sleep_for(1ms);
            probe.active.fetch_sub(1);
            probe.writing.store(false);
            ++v;
        }));
        ++writes;
    }

    for (auto& t : tasks)
        t.get();

    ts::Task<int> final_value = data.async([](const int& v) { return v; });
    int v = final_value.get();

    bool ok = !probe.violated.load() && v == writes;
    std::printf("  writer exclusion: overlap = %s, value = %d (expected %d) -> %s\n",
        probe.violated.load() ? "yes" : "no", v, writes, ok ? "ok" : "FAIL");
    return ok;
}

// then() chains continuations: read -> double -> +1, plus a void sink.
bool test_continuations()
{
    ts::Thread_safe<int> data{ 21 };

    ts::Task<int> chained =
        data.async([](const int& v) { return v; })
            .then([](int v) { return v * 2; })
            .then([](int v) { return v + 1; });

    int v = chained.get();

    std::atomic<int> sink{ 0 };
    data.async([](const int& x) { return x; })
        .then([&sink](int x) { sink.store(x + 100); })
        .get();

    bool ok = (v == 43) && (sink.load() == 121);
    std::printf("  continuations: chain = %d (expected 43), sink = %d (expected 121) -> %s\n",
        v, sink.load(), ok ? "ok" : "FAIL");
    return ok;
}

int read_value(ts::Thread_safe<int>& data)
{
    return data.async([](const int& v) { return v; }).get();
}

// Access conflicts derive ordering: sim(W a) < build(R a, W b) < draw(R a, R b, W c).
bool test_graph_access_ordering()
{
    ts::Thread_safe<int> a{ 0 }, b{ 0 }, c{ 0 };

    ts::Static_task_graph g;
    g.add_node([](int& x) { x = 1; }, a);
    g.add_node([](const int& x, int& y) { y = x * 10; }, a, b);
    g.add_node([](const int& x, const int& y, int& z) { z = x + y; }, a, b, c);
    g.compile();

    g.execute().get();
    int av = read_value(a), bv = read_value(b), cv = read_value(c);

    g.execute().get();   // re-run: deterministic
    int cv2 = read_value(c);

    bool ok = av == 1 && bv == 10 && cv == 11 && cv2 == 11;
    std::printf("  graph access ordering: a=%d b=%d c=%d (1/10/11), rerun c=%d -> %s\n",
        av, bv, cv, cv2, ok ? "ok" : "FAIL");
    return ok;
}

// Explicit after() orders two otherwise-independent nodes.
bool test_graph_explicit_ordering()
{
    ts::Thread_safe<int> p{ 0 }, q{ 0 };
    std::atomic<int> seq{ 0 };
    std::atomic<int> p_order{ 0 }, q_order{ 0 };

    ts::Static_task_graph g;
    ts::Task<void> np = g.add_node([&seq, &p_order](int&) { p_order.store(++seq); }, p);
    ts::Task<void> nq = g.add_node([&seq, &q_order](int&) { q_order.store(++seq); }, q);
    nq.after(np);
    g.compile();

    g.execute().get();

    bool ok = p_order.load() == 1 && q_order.load() == 2;
    std::printf("  graph explicit ordering: p=%d q=%d (1/2) -> %s\n",
        p_order.load(), q_order.load(), ok ? "ok" : "FAIL");
    return ok;
}

} // namespace

void run_thread_safe_tests()
{
    std::printf("\n[thread_safe] tests\n");

    bool ok = true;
    ok &= test_serial_correctness();
    ok &= test_concurrent_readers();
    ok &= test_writer_exclusion();
    ok &= test_continuations();
    ok &= test_graph_access_ordering();
    ok &= test_graph_explicit_ordering();

    std::printf("  [thread_safe] %s\n", ok ? "ALL OK" : "FAILURES");
}
