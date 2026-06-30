#include "task_tests.h"
#include "thread_safe.h"
#include "harness.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <tuple>

using namespace std::chrono_literals;
using ts::test::run;
using tests::wait_until;

namespace
{

ts::Task<int> read_async(ts::Thread_safe<int>& d)
{
    return d.async([](const int& v) { return v; });
}

// --- E: `get` / `is_done` / `then` ----------------------------------------

void test_get_void()
{
    ts::Thread_safe<int> d{ 0 };
    ts::Task<void> t = d.async([](int& v) { v = 1; });
    t.get();
    TS_CHECK(read_async(d).get() == 1);
}

void test_is_done()
{
    std::atomic<bool> go{ false };
    ts::Thread_safe<int> d{ 5 };
    ts::Task<int> t = d.async([&go](const int& v)
    {
        wait_until([&] { return go.load(); });
        return v;
    });

    TS_CHECK(!t.is_done());     // job is blocked on `go`
    go.store(true);
    TS_CHECK(t.get() == 5);
    TS_CHECK(t.is_done());
}

void test_then_single()
{
    ts::Thread_safe<int> d{ 20 };
    int r = read_async(d).then([](int v) { return v + 1; }).get();
    TS_CHECK(r == 21);
}

void test_then_chain()
{
    ts::Thread_safe<int> d{ 21 };
    int r = read_async(d)
                .then([](int v) { return v * 2; })
                .then([](int v) { return v + 1; })
                .get();
    TS_CHECK(r == 43);
}

void test_then_void_producer()
{
    ts::Thread_safe<int> d{ 0 };
    int r = d.async([](int& v) { v = 7; }).then([] { return 100; }).get();
    TS_CHECK(r == 100);
}

void test_then_void_result()
{
    std::atomic<int> sink{ 0 };
    ts::Thread_safe<int> d{ 9 };
    read_async(d).then([&sink](int v) { sink.store(v); }).get();
    TS_CHECK(sink.load() == 9);
}

void test_then_after_completion()
{
    ts::Thread_safe<int> d{ 3 };
    ts::Task<int> t = read_async(d);
    wait_until([&] { return t.is_done(); });
    int r = t.then([](int v) { return v * 10; }).get();   // attached after completion
    TS_CHECK(r == 30);
}

// --- F: `when_all` --------------------------------------------------------

void test_when_all_two()
{
    ts::Thread_safe<int> a{ 10 }, b{ 32 };
    int s = ts::when_all(read_async(a), read_async(b))
                .then([](std::tuple<int, int>& r) { return std::get<0>(r) + std::get<1>(r); })
                .get();
    TS_CHECK(s == 42);
}

void test_when_all_three()
{
    ts::Thread_safe<int> a{ 1 }, b{ 2 }, c{ 3 };
    int s = ts::when_all(read_async(a), read_async(b), read_async(c))
                .then([](std::tuple<int, int, int>& r)
                {
                    return std::get<0>(r) + std::get<1>(r) + std::get<2>(r);
                })
                .get();
    TS_CHECK(s == 6);
}

void test_when_all_single()
{
    ts::Thread_safe<int> a{ 7 };
    int s = ts::when_all(read_async(a))
                .then([](std::tuple<int>& r) { return std::get<0>(r); })
                .get();
    TS_CHECK(s == 7);
}

void test_when_all_out_of_order()
{
    ts::Thread_safe<int> a{ 1 }, b{ 2 };
    ts::Task<int> ta = a.async([](const int& v) { std::this_thread::sleep_for(20ms); return v; });
    ts::Task<int> tb = b.async([](const int& v) { return v; });   // completes first

    int s = ts::when_all(ta, tb)
                .then([](std::tuple<int, int>& r) { return std::get<0>(r) + std::get<1>(r); })
                .get();
    TS_CHECK(s == 3);
}

void test_when_all_already_complete()
{
    ts::Thread_safe<int> a{ 4 }, b{ 5 };
    ts::Task<int> ta = read_async(a);
    ts::Task<int> tb = read_async(b);
    wait_until([&] { return ta.is_done() && tb.is_done(); });

    int s = ts::when_all(ta, tb)
                .then([](std::tuple<int, int>& r) { return std::get<0>(r) + std::get<1>(r); })
                .get();
    TS_CHECK(s == 9);
}

void test_when_all_nested()
{
    ts::Thread_safe<int> a{ 1 }, b{ 2 }, c{ 3 };
    ts::Task<int> ab = ts::when_all(read_async(a), read_async(b))
                           .then([](std::tuple<int, int>& r) { return std::get<0>(r) + std::get<1>(r); });

    int s = ts::when_all(ab, read_async(c))
                .then([](std::tuple<int, int>& r) { return std::get<0>(r) + std::get<1>(r); })
                .get();
    TS_CHECK(s == 6);
}

} // namespace

void run_task_tests()
{
    std::printf("\n[task] tests\n");
    run("get void", test_get_void);
    run("is_done", test_is_done);
    run("then single", test_then_single);
    run("then chain", test_then_chain);
    run("then void producer", test_then_void_producer);
    run("then void result", test_then_void_result);
    run("then after completion", test_then_after_completion);
    run("when_all two", test_when_all_two);
    run("when_all three", test_when_all_three);
    run("when_all single", test_when_all_single);
    run("when_all out of order", test_when_all_out_of_order);
    run("when_all already complete", test_when_all_already_complete);
    run("when_all nested", test_when_all_nested);
}
