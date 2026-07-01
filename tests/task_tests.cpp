#include "task_tests.h"
#include "thread_safe.h"
#include "harness.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <tuple>
#include <vector>

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

// --- F2: `when_all` completeness (void, move-only, apply-style) ------------

void test_when_all_void_prereq()
{
    ts::Thread_safe<int> a{ 10 }, b{ 32 };
    std::atomic<int> side{ 0 };
    ts::Task<void> v = a.async([&side](int& x) { side.store(x); });   // void prerequisite
    ts::Task<int> r = b.async([](const int& x) { return x; });

    int got = ts::when_all(v, r).then([](std::tuple<int>& t) { return std::get<0>(t); }).get();
    TS_CHECK(got == 32);            // only the non-void result is carried
    TS_CHECK(side.load() == 10);    // the void prerequisite ran
}

void test_when_all_all_void()
{
    ts::Thread_safe<int> a{ 0 }, b{ 0 };
    std::atomic<int> count{ 0 };
    ts::Task<void> ta = a.async([&count](int&) { count.fetch_add(1); });
    ts::Task<void> tb = b.async([&count](int&) { count.fetch_add(1); });

    ts::when_all(ta, tb).get();     // all-void join -> Task<void>
    TS_CHECK(count.load() == 2);
}

void test_when_all_move_only()
{
    ts::Thread_safe<int> a{ 5 }, b{ 7 };
    ts::Task<std::unique_ptr<int>> pa = a.async([](const int& x) { return std::make_unique<int>(x); });
    ts::Task<std::unique_ptr<int>> pb = b.async([](const int& x) { return std::make_unique<int>(x); });

    int sum = ts::when_all(pa, pb)
        .then([](std::tuple<std::unique_ptr<int>, std::unique_ptr<int>>& t)
        {
            return *std::get<0>(t) + *std::get<1>(t);
        })
        .get();
    TS_CHECK(sum == 12);
}

void test_when_all_apply_style()
{
    ts::Thread_safe<int> a{ 10 }, b{ 32 };
    int sum = ts::when_all(
            a.async([](const int& x) { return x; }),
            b.async([](const int& x) { return x; }))
        .then([](int x, int y) { return x + y; })   // unpacked, not a tuple
        .get();
    TS_CHECK(sum == 42);
}

void test_when_all_apply_void()
{
    ts::Thread_safe<int> a{ 1 }, b{ 2 };
    std::atomic<int> sink{ 0 };
    ts::when_all(
            a.async([](const int& x) { return x; }),
            b.async([](const int& x) { return x; }))
        .then([&sink](int x, int y) { sink.store(x + y); })   // unpacked, void result
        .get();
    TS_CHECK(sink.load() == 3);
}

// --- G: `Signal` ----------------------------------------------------------

void test_signal_trigger_then_wait()
{
    ts::Signal s;
    TS_CHECK(!s.is_done());
    s.trigger();
    s.get();                    // does not block: already triggered
    TS_CHECK(s.is_done());
}

void test_signal_wait_then_trigger()
{
    ts::Signal s;
    std::atomic<bool> woke{ false };
    std::jthread producer([&]
    {
        std::this_thread::sleep_for(10ms);
        s.trigger();
    });

    s.get();                    // blocks until triggered on the other thread
    woke.store(true);
    TS_CHECK(woke.load());
    TS_CHECK(s.is_done());
}

void test_signal_then()
{
    ts::Signal s;
    std::atomic<int> sink{ 0 };
    ts::Task<void> t = s.then([&sink] { sink.store(42); });
    TS_CHECK(sink.load() == 0);  // continuation waits for the trigger
    s.trigger();
    t.get();
    TS_CHECK(sink.load() == 42);
}

void test_signal_then_after_trigger()
{
    ts::Signal s;
    s.trigger();
    std::atomic<int> sink{ 0 };
    s.then([&sink] { sink.store(7); }).get();   // attached after completion: runs inline
    TS_CHECK(sink.load() == 7);
}

void test_signal_idempotent_trigger()
{
    ts::Signal s;
    std::atomic<int> fired{ 0 };
    s.then([&fired] { fired.fetch_add(1); });

    // Many concurrent triggers must complete the signal exactly once.
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < 8; ++i)
            threads.emplace_back([&] { s.trigger(); });
    }
    s.get();
    TS_CHECK(fired.load() == 1);
}

void test_signal_copies_share_state()
{
    ts::Signal s;
    ts::Signal copy = s;        // shares one control block
    s.trigger();
    copy.get();
    TS_CHECK(copy.is_done() && s.is_done());
}

void test_signal_multiple_waiters()
{
    ts::Signal s;
    std::atomic<int> woke{ 0 };
    {
        std::vector<std::jthread> waiters;
        for (int i = 0; i < 8; ++i)
            waiters.emplace_back([&] { s.get(); woke.fetch_add(1); });   // all block on one signal
        std::this_thread::sleep_for(5ms);   // let them park
        TS_CHECK(woke.load() == 0);
        s.trigger();
    }   // join: every waiter must have woken
    TS_CHECK(woke.load() == 8);
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
    run("when_all void prereq", test_when_all_void_prereq);
    run("when_all all void", test_when_all_all_void);
    run("when_all move-only", test_when_all_move_only);
    run("when_all apply style", test_when_all_apply_style);
    run("when_all apply void", test_when_all_apply_void);
    run("signal trigger then wait", test_signal_trigger_then_wait);
    run("signal wait then trigger", test_signal_wait_then_trigger);
    run("signal then", test_signal_then);
    run("signal then after trigger", test_signal_then_after_trigger);
    run("signal idempotent trigger", test_signal_idempotent_trigger);
    run("signal copies share state", test_signal_copies_share_state);
    run("signal multiple waiters", test_signal_multiple_waiters);
}
