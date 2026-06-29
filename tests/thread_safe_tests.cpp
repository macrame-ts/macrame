#include "thread_safe_tests.h"
#include "thread_safe.h"
#include "harness.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using ts::test::run;
using tests::Counter;
using tests::record_max;
using tests::wait_until;

namespace
{

int read_value(ts::Thread_safe<int>& data)
{
    return data.async([](const int& v) { return v; }).get();
}

// --- C: compile-time constraints ------------------------------------------

struct Write_fn { void operator()(int&) const {} };
struct Read_fn  { void operator()(const int&) const {} };
struct Int_read { int operator()(const int&) const { return 0; } };
struct Int_write { int operator()(int&) const { return 0; } };

template<typename T, typename Fn>
concept Async_on_mutable = requires(ts::Thread_safe<T>& t, Fn fn) { t.async(fn); };

template<typename T, typename Fn>
concept Async_on_const = requires(const ts::Thread_safe<T>& t, Fn fn) { t.async(fn); };

static_assert(Async_on_mutable<int, Write_fn>);
static_assert(Async_on_mutable<int, Read_fn>);
static_assert(Async_on_const<int, Read_fn>);
static_assert(!Async_on_const<int, Write_fn>, "write must not be callable through a const handle");
static_assert(!std::is_copy_constructible_v<ts::Thread_safe<int>>);
static_assert(!std::is_move_constructible_v<ts::Thread_safe<int>>);
static_assert(std::is_same_v<
    decltype(std::declval<ts::Thread_safe<int>&>().async(std::declval<Int_read>())), ts::Task<int>>);
static_assert(std::is_same_v<
    decltype(std::declval<ts::Thread_safe<int>&>().async(std::declval<Int_write>())), ts::Task<int>>);

void test_type_constraints()
{
    TS_CHECK((Async_on_mutable<int, Write_fn>));
    TS_CHECK((Async_on_const<int, Read_fn>));
    TS_CHECK((!Async_on_const<int, Write_fn>));
    TS_CHECK(!std::is_copy_constructible_v<ts::Thread_safe<int>>);
    TS_CHECK(!std::is_move_constructible_v<ts::Thread_safe<int>>);
}

// --- C: basics ------------------------------------------------------------

void test_construct()
{
    ts::Thread_safe<int> d{ 5 };
    TS_CHECK(read_value(d) == 5);
}

void test_write_then_read()
{
    ts::Thread_safe<Counter> c;
    c.async([](Counter& x) { x.add(7); });
    int v = c.async([](const Counter& x) { return x.value(); }).get();
    TS_CHECK(v == 7);
}

void test_async_returns_value()
{
    ts::Thread_safe<int> d{ 41 };
    ts::Task<int> t = d.async([](const int& v) { return v + 1; });
    TS_CHECK(t.get() == 42);
}

void test_destructor_waits()
{
    constexpr int count = 1000;
    std::atomic<int> done{ 0 };
    {
        ts::Thread_safe<int> d{ 0 };
        for (int i = 0; i < count; ++i)
            d.async([&done](int& v) { ++v; done.fetch_add(1); });
    }   // destructor waits for the pipe to drain
    TS_CHECK(done.load() == count);
}

// --- D: reader/writer pipe ------------------------------------------------

void test_serial_correctness()
{
    ts::Thread_safe<Counter> counter;
    for (int i = 0; i < 1000; ++i)
        counter.async([](Counter& c) { c.increment(); });
    int v = counter.async([](const Counter& c) { return c.value(); }).get();
    TS_CHECK(v == 1000);
}

void test_concurrent_readers()
{
    std::atomic<int> active{ 0 }, peak{ 0 };
    ts::Thread_safe<int> data{ 7 };
    std::vector<ts::Task<int>> tasks;

    for (int i = 0; i < 16; ++i)
        tasks.push_back(data.async([&active, &peak](const int& v)
        {
            record_max(peak, active.fetch_add(1) + 1);
            std::this_thread::sleep_for(3ms);
            active.fetch_sub(1);
            return v;
        }));

    for (auto& t : tasks)
        t.get();
    TS_CHECK(peak.load() > 1);
}

void test_writer_exclusion()
{
    std::atomic<int> active{ 0 };
    std::atomic<bool> writing{ false }, violated{ false };
    ts::Thread_safe<int> data{ 0 };
    std::vector<ts::Task<void>> tasks;
    int writes = 0;

    for (int round = 0; round < 20; ++round)
    {
        for (int r = 0; r < 4; ++r)
            tasks.push_back(data.async([&active, &writing, &violated](const int&)
            {
                active.fetch_add(1);
                if (writing.load())
                    violated.store(true);
                std::this_thread::sleep_for(1ms);
                active.fetch_sub(1);
            }));

        tasks.push_back(data.async([&active, &writing, &violated](int& v)
        {
            writing.store(true);
            if (active.fetch_add(1) + 1 != 1)
                violated.store(true);
            std::this_thread::sleep_for(1ms);
            active.fetch_sub(1);
            writing.store(false);
            ++v;
        }));
        ++writes;
    }

    for (auto& t : tasks)
        t.get();
    TS_CHECK(!violated.load());
    TS_CHECK(read_value(data) == writes);
}

void test_reader_after_writer()
{
    ts::Thread_safe<int> data{ 0 };
    data.async([](int& v) { v = 99; });
    TS_CHECK(read_value(data) == 99);   // FIFO: the read sees the prior write
}

void test_independent_objects_parallel()
{
    tests::Parallel_gate gate{ 2 };
    ts::Thread_safe<int> a{ 0 }, b{ 0 };

    auto job = [&gate](int&) { gate.arrive(); };
    ts::Task<void> ta = a.async(job);
    ts::Task<void> tb = b.async(job);
    ta.get();
    tb.get();
    TS_CHECK(gate.met());   // separate pipes ran concurrently
}

void test_reentrant_same_object()
{
    ts::Thread_safe<int> d{ 0 };
    std::atomic<int> done{ 0 };

    d.async([&d, &done](int& v)
    {
        v += 1;
        d.async([&done](int& w) { w += 1; done.fetch_add(1); });   // same pipe
        done.fetch_add(1);
    });

    wait_until([&] { return done.load() == 2; });
    TS_CHECK(read_value(d) == 2);
}

void test_reentrant_other_object()
{
    ts::Thread_safe<int> a{ 0 }, b{ 0 };
    std::atomic<int> done{ 0 };

    a.async([&b, &done](int& v)
    {
        v = 1;
        b.async([&done](int& w) { w = 1; done.fetch_add(1); });
        done.fetch_add(1);
    });

    wait_until([&] { return done.load() == 2; });
    TS_CHECK(read_value(a) == 1 && read_value(b) == 1);
}

void test_pipe_stress()
{
    ts::Thread_safe<int> d{ 0 };
    for (int i = 0; i < 5000; ++i)
        d.async([](int& v) { ++v; });
    TS_CHECK(read_value(d) == 5000);
}

} // namespace

void run_thread_safe_tests()
{
    std::printf("\n[thread_safe] tests\n");
    run("type constraints", test_type_constraints);
    run("construct", test_construct);
    run("write then read", test_write_then_read);
    run("async returns value", test_async_returns_value);
    run("destructor waits", test_destructor_waits);
    run("serial correctness", test_serial_correctness);
    run("concurrent readers", test_concurrent_readers);
    run("writer exclusion", test_writer_exclusion);
    run("reader after writer", test_reader_after_writer);
    run("independent objects parallel", test_independent_objects_parallel);
    run("reentrant same object", test_reentrant_same_object);
    run("reentrant other object", test_reentrant_other_object);
    run("pipe stress", test_pipe_stress);
}
