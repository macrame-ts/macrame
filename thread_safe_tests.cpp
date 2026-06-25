#include "thread_safe_tests.h"
#include "thread_safe.h"

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

} // namespace

void run_thread_safe_tests()
{
    std::printf("\n[thread_safe] tests\n");

    bool ok = true;
    ok &= test_serial_correctness();
    ok &= test_concurrent_readers();
    ok &= test_writer_exclusion();

    std::printf("  [thread_safe] %s\n", ok ? "ALL OK" : "FAILURES");
}
