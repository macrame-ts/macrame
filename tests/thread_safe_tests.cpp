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
using tests::wait_until;

namespace
{

int read_value(ts::Thread_safe<int>& data)
{
    return data.async([](const int& v) { return v; }).sync();
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
    int v = c.async([](const Counter& x) { return x.value(); }).sync();
    TS_CHECK(v == 7);
}

void test_async_returns_value()
{
    ts::Thread_safe<int> d{ 41 };
    ts::Task<int> t = d.async([](const int& v) { return v + 1; });
    TS_CHECK(t.sync() == 42);
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
    int v = counter.async([](const Counter& c) { return c.value(); }).sync();
    TS_CHECK(v == 1000);
}

void test_concurrent_readers()
{
    // Deterministic concurrency check: two readers each wait (bounded) for the other, so
    // the gate is met iff the pipe genuinely ran readers concurrently -- rather than the
    // old "peak > 1" that merely hoped the timing overlapped.
    tests::Parallel_gate gate{ 2 };
    ts::Thread_safe<int> data{ 7 };
    std::vector<ts::Task<int>> tasks;

    for (int i = 0; i < 16; ++i)
        tasks.push_back(data.async([&gate](const int& v)
        {
            gate.arrive();
            return v;
        }));

    for (auto& t : tasks)
        t.sync();
    TS_CHECK(gate.met());   // concurrent readers (not serialized by the reader/writer pipe)
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
        t.sync();
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
    ta.sync();
    tb.sync();
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

// --- inline dispatch (Task_options::run_inline) --------------------------

// A write accessor with `run_inline` on a free pipe runs synchronously on the CALLING
// thread; the returned task is already settled when the call returns.
void test_inline_runs_synchronously()
{
    ts::Thread_safe<int> d{ 5 };
    std::thread::id body_thread{};
    ts::Task<int> t = d.async([&body_thread](int& v)
    {
        body_thread = std::this_thread::get_id();
        return ++v;   // 6
    }, { .run_inline = true });

    TS_CHECK(t.is_done());                                  // ran before this line returned
    TS_CHECK(body_thread == std::this_thread::get_id());    // on the calling thread
    TS_CHECK(t.sync() == 6);
    TS_CHECK(read_value(d) == 6);
}

// A read accessor with `run_inline` on a free pipe joins as a reader and runs on the caller.
void test_inline_read_on_caller()
{
    ts::Thread_safe<int> d{ 9 };
    std::thread::id body_thread{};
    int r = d.async([&body_thread](const int& v)
    {
        body_thread = std::this_thread::get_id();
        return v;
    }, { .run_inline = true }).sync();
    TS_CHECK(r == 9);
    TS_CHECK(body_thread == std::this_thread::get_id());
}

// When the pipe is busy (a writer holds it), an inline-requested async cannot acquire it,
// so it defers to the queue and runs correctly on a worker once the pipe drains.
void test_inline_falls_back_when_busy()
{
    ts::Thread_safe<int> d{ 0 };
    std::atomic<bool> gate{ false };
    std::thread::id caller = std::this_thread::get_id();
    std::atomic<std::thread::id> inline_thread{};

    // Occupy the pipe with a writer that holds it until `gate` (writer_active is set
    // synchronously by dispatch, so the pipe is busy by the time this call returns).
    ts::Task<void> blocker = d.async([&gate](int&)
    {
        while (!gate.load()) std::this_thread::yield();
    });

    // Request inline while the writer holds the pipe -> must defer (not run on the caller).
    ts::Task<int> t = d.async([&inline_thread](int& v)
    {
        inline_thread.store(std::this_thread::get_id());
        return ++v;
    }, { .run_inline = true });

    TS_CHECK(!t.is_done());          // deferred behind the blocker, not run inline

    gate.store(true);
    TS_CHECK(t.sync() == 1);          // ran correctly after the blocker drained
    TS_CHECK(inline_thread.load() != caller);   // on a worker, not the calling thread
    blocker.sync();
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
    run("inline runs synchronously", test_inline_runs_synchronously);
    run("inline read on caller", test_inline_read_on_caller);
    run("inline falls back when busy", test_inline_falls_back_when_busy);
}
