#include "guarded_tests.h"
#include "ts/guarded.h"
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

int read_value(ts::Guarded<int>& data)
{
    return data.async([](const int& v) { return v; }).sync();
}

// --- C: compile-time constraints ------------------------------------------

struct Write_fn { void operator()(int&) const {} };
struct Read_fn  { void operator()(const int&) const {} };
struct Int_read { int operator()(const int&) const { return 0; } };
struct Int_write { int operator()(int&) const { return 0; } };

template<typename T, typename Fn>
concept Async_on_mutable = requires(ts::Guarded<T>& t, Fn fn) { t.async(fn); };

template<typename T, typename Fn>
concept Async_on_const = requires(const ts::Guarded<T>& t, Fn fn) { t.async(fn); };

static_assert(Async_on_mutable<int, Write_fn>);
static_assert(Async_on_mutable<int, Read_fn>);
static_assert(Async_on_const<int, Read_fn>);
static_assert(!Async_on_const<int, Write_fn>, "write must not be callable through a const handle");
static_assert(!std::is_copy_constructible_v<ts::Guarded<int>>);
static_assert(!std::is_move_constructible_v<ts::Guarded<int>>);

// Generic (probed) classification: `auto&` = write (an rvalue cannot bind it), so it must not
// be callable through a const handle; `const auto&` = read, so it must be.
using Generic_write = decltype([](auto& v) { ++v; });
using Generic_read = decltype([](const auto& v) { (void)v; });
static_assert(Async_on_mutable<int, Generic_write>);
static_assert(!Async_on_const<int, Generic_write>, "generic write probes read_write");
static_assert(Async_on_const<int, Generic_read>, "generic const read probes read_only");

#if 0   // Compile-time rejections, kept for documentation -- each of these must NOT compile:
void must_not_compile(ts::Guarded<int>& g)
{
    g.async([](int v) { ++v; });            // by-value resource param: static_assert (a copy
                                            // would silently discard the writes)
    g.async([](int&& v) { (void)v; });      // rvalue-ref resource param: static_assert
    g.async([](const auto& v) { ++v; });    // mutating body under a read classification:
                                            // read bodies receive `const T&`
}
#endif
static_assert(std::is_same_v<
    decltype(std::declval<ts::Guarded<int>&>().async(std::declval<Int_read>())), ts::Task<int>>);
static_assert(std::is_same_v<
    decltype(std::declval<ts::Guarded<int>&>().async(std::declval<Int_write>())), ts::Task<int>>);

void test_type_constraints()
{
    TS_CHECK((Async_on_mutable<int, Write_fn>));
    TS_CHECK((Async_on_const<int, Read_fn>));
    TS_CHECK((!Async_on_const<int, Write_fn>));
    TS_CHECK(!std::is_copy_constructible_v<ts::Guarded<int>>);
    TS_CHECK(!std::is_move_constructible_v<ts::Guarded<int>>);
}

// --- C: basics ------------------------------------------------------------

void test_construct()
{
    ts::Guarded<int> d{ 5 };
    TS_CHECK(read_value(d) == 5);
}

void test_write_then_read()
{
    ts::Guarded<Counter> c;
    c.async([](Counter& x) { x.add(7); });
    int v = c.async([](const Counter& x) { return x.value(); }).sync();
    TS_CHECK(v == 7);
}

void test_async_returns_value()
{
    ts::Guarded<int> d{ 41 };
    ts::Task<int> t = d.async([](const int& v) { return v + 1; });
    TS_CHECK(t.sync() == 42);
}

void test_destructor_waits()
{
    constexpr int count = 1000;
    std::atomic<int> done{ 0 };
    {
        ts::Guarded<int> d{ 0 };
        for (int i = 0; i < count; ++i)
            d.async([&done](int& v) { ++v; done.fetch_add(1); });
    }   // destructor waits for the pipe to drain
    TS_CHECK(done.load() == count);
}

// --- D: reader/writer pipe ------------------------------------------------

void test_serial_correctness()
{
    ts::Guarded<Counter> counter;
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
    ts::Guarded<int> data{ 7 };
    std::vector<ts::Task<int>> tasks;

    for (int i = 0; i < 16; ++i)
    {
        tasks.push_back(data.async([&gate](const int& v)
        {
            gate.arrive();
            return v;
        }));
    }

    for (auto& t : tasks)
        t.sync();
    TS_CHECK(gate.met());   // concurrent readers (not serialized by the reader/writer pipe)
}

void test_writer_exclusion()
{
    std::atomic<int> active{ 0 };
    std::atomic<bool> writing{ false }, violated{ false };
    ts::Guarded<int> data{ 0 };
    std::vector<ts::Task<void>> tasks;
    int writes = 0;

    for (int round = 0; round < 20; ++round)
    {
        for (int r = 0; r < 4; ++r)
        {
            tasks.push_back(data.async([&active, &writing, &violated](const int&)
            {
                active.fetch_add(1);
                if (writing.load())
                    violated.store(true);
                std::this_thread::sleep_for(1ms);
                active.fetch_sub(1);
            }));
        }

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
    ts::Guarded<int> data{ 0 };
    data.async([](int& v) { v = 99; });
    TS_CHECK(read_value(data) == 99);   // FIFO: the read sees the prior write
}

void test_independent_objects_parallel()
{
    tests::Parallel_gate gate{ 2 };
    ts::Guarded<int> a{ 0 }, b{ 0 };

    auto job = [&gate](int&) { gate.arrive(); };
    ts::Task<void> ta = a.async(job);
    ts::Task<void> tb = b.async(job);
    ta.sync();
    tb.sync();
    TS_CHECK(gate.met());   // separate pipes ran concurrently
}

// --- D: generic (probed) accessors ----------------------------------------
// No tags: a generic `[](const auto&)` probes read_only (it binds an rvalue), `[](auto&)`
// probes read_write (it cannot). Same admission behavior as the spelled-out forms.

void test_generic_readers_overlap()
{
    tests::Parallel_gate gate{ 2 };
    ts::Guarded<int> data{ 7 };
    std::vector<ts::Task<int>> tasks;

    for (int i = 0; i < 8; ++i)
    {
        tasks.push_back(data.async([&gate](const auto& v)
        {
            gate.arrive();
            return v;
        }));
    }

    for (auto& t : tasks)
        t.sync();
    TS_CHECK(gate.met());   // probed read_only: readers ran concurrently
}

void test_generic_writer_serializes()
{
    ts::Guarded<int> d{ 0 };
    for (int i = 0; i < 200; ++i)
        d.async([](auto& v) { ++v; });   // probed read_write: exclusive, all land
    TS_CHECK(read_value(d) == 200);
}

void test_reentrant_same_object()
{
    ts::Guarded<int> d{ 0 };
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
    ts::Guarded<int> a{ 0 }, b{ 0 };
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
    ts::Guarded<int> d{ 0 };
    for (int i = 0; i < 5000; ++i)
        d.async([](int& v) { ++v; });
    TS_CHECK(read_value(d) == 5000);
}

// --- access (inline-when-free) vs async (always enqueued) ----------------

// A write `access` on a free pipe runs synchronously on the CALLING thread; the returned task
// is already settled when the call returns.
void test_access_runs_synchronously()
{
    ts::Guarded<int> d{ 5 };
    std::thread::id body_thread{};
    ts::Task<int> t = d.access([&body_thread](int& v)
    {
        body_thread = std::this_thread::get_id();
        return ++v;   // 6
    });

    TS_CHECK(t.is_done());                                  // ran before this line returned
    TS_CHECK(body_thread == std::this_thread::get_id());    // on the calling thread
    TS_CHECK(t.sync() == 6);
    TS_CHECK(read_value(d) == 6);
}

// A read `access` on a free pipe joins as a reader and runs on the caller.
void test_access_read_on_caller()
{
    ts::Guarded<int> d{ 9 };
    std::thread::id body_thread{};
    int r = d.access([&body_thread](const int& v)
    {
        body_thread = std::this_thread::get_id();
        return v;
    }).sync();
    TS_CHECK(r == 9);
    TS_CHECK(body_thread == std::this_thread::get_id());
}

// When the pipe is busy (a writer holds it), `access` cannot acquire it, so it defers to the
// queue and runs correctly on a worker once the pipe drains.
void test_access_falls_back_when_busy()
{
    ts::Guarded<int> d{ 0 };
    std::atomic<bool> gate{ false };
    std::thread::id caller = std::this_thread::get_id();
    std::atomic<std::thread::id> body_thread{};

    // Occupy the pipe with a writer that holds it until `gate` (writer_active is set
    // synchronously by dispatch, so the pipe is busy by the time this call returns).
    ts::Task<void> blocker = d.async([&gate](int&)
    {
        while (!gate.load()) std::this_thread::yield();
    });

    // `access` while the writer holds the pipe -> must defer (not run on the caller).
    ts::Task<int> t = d.access([&body_thread](int& v)
    {
        body_thread.store(std::this_thread::get_id());
        return ++v;
    });

    TS_CHECK(!t.is_done());          // deferred behind the blocker, not run inline

    gate.store(true);
    TS_CHECK(t.sync() == 1);          // ran correctly after the blocker drained
    TS_CHECK(body_thread.load() != caller);   // on a worker, not the calling thread
    blocker.sync();
}

// `async` always enqueues -- even on a free pipe it does NOT run on the calling thread.
// Asserted via the executing thread id: an inline regression would run the body on the
// caller (synchronously, before `async` returns), failing the id check deterministically.
// A `!t.is_done()` probe after `async` returns is NOT sound -- a fast worker can complete
// the body before the check runs (it cost a CI flake on a slow runner).
void test_async_always_schedules()
{
    ts::Guarded<int> d{ 0 };
    std::thread::id caller = std::this_thread::get_id();
    std::atomic<std::thread::id> body_thread{ caller };
    ts::Task<int> t = d.async([&body_thread](int& v)
    {
        body_thread.store(std::this_thread::get_id());
        return ++v;
    });
    TS_CHECK(t.sync() == 1);
    TS_CHECK(body_thread.load() != caller);    // on a worker, never inline on the caller
    TS_CHECK(read_value(d) == 1);
}

// A read `async` also always enqueues and still deduces read (concurrent) access.
void test_async_read_schedules()
{
    ts::Guarded<int> d{ 7 };
    TS_CHECK(d.async([](const int& v) { return v; }).sync() == 7);
}

} // namespace

void run_guarded_tests()
{
    std::printf("\n[guarded] tests\n");
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
    run("generic readers overlap", test_generic_readers_overlap);
    run("generic writer serializes", test_generic_writer_serializes);
    run("reentrant same object", test_reentrant_same_object);
    run("reentrant other object", test_reentrant_other_object);
    run("pipe stress", test_pipe_stress);
    run("access runs synchronously", test_access_runs_synchronously);
    run("access read on caller", test_access_read_on_caller);
    run("access falls back when busy", test_access_falls_back_when_busy);
    run("async always schedules", test_async_always_schedules);
    run("async read schedules", test_async_read_schedules);
}
