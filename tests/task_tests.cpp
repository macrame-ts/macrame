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
    t.sync();
    TS_CHECK(read_async(d).sync() == 1);
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
    TS_CHECK(t.sync() == 5);
    TS_CHECK(t.is_done());
}

void test_then_single()
{
    ts::Thread_safe<int> d{ 20 };
    int r = read_async(d).then([](int v) { return v + 1; }).sync();
    TS_CHECK(r == 21);
}

void test_then_chain()
{
    ts::Thread_safe<int> d{ 21 };
    int r = read_async(d)
                .then([](int v) { return v * 2; })
                .then([](int v) { return v + 1; })
                .sync();
    TS_CHECK(r == 43);
}

void test_then_void_producer()
{
    ts::Thread_safe<int> d{ 0 };
    int r = d.async([](int& v) { v = 7; }).then([] { return 100; }).sync();
    TS_CHECK(r == 100);
}

void test_then_void_result()
{
    std::atomic<int> sink{ 0 };
    ts::Thread_safe<int> d{ 9 };
    read_async(d).then([&sink](int v) { sink.store(v); }).sync();
    TS_CHECK(sink.load() == 9);
}

void test_then_after_completion()
{
    ts::Thread_safe<int> d{ 3 };
    ts::Task<int> t = read_async(d);
    wait_until([&] { return t.is_done(); });
    int r = t.then([](int v) { return v * 10; }).sync();   // attached after completion
    TS_CHECK(r == 30);
}

// --- F: `when_all` --------------------------------------------------------

void test_when_all_two()
{
    ts::Thread_safe<int> a{ 10 }, b{ 32 };
    int s = ts::when_all(read_async(a), read_async(b))
                .then([](std::tuple<int, int>& r) { return std::get<0>(r) + std::get<1>(r); })
                .sync();
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
                .sync();
    TS_CHECK(s == 6);
}

void test_when_all_single()
{
    ts::Thread_safe<int> a{ 7 };
    int s = ts::when_all(read_async(a))
                .then([](std::tuple<int>& r) { return std::get<0>(r); })
                .sync();
    TS_CHECK(s == 7);
}

void test_when_all_out_of_order()
{
    ts::Thread_safe<int> a{ 1 }, b{ 2 };
    ts::Task<int> ta = a.async([](const int& v) { std::this_thread::sleep_for(20ms); return v; });
    ts::Task<int> tb = b.async([](const int& v) { return v; });   // completes first

    int s = ts::when_all(ta, tb)
                .then([](std::tuple<int, int>& r) { return std::get<0>(r) + std::get<1>(r); })
                .sync();
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
                .sync();
    TS_CHECK(s == 9);
}

void test_when_all_nested()
{
    ts::Thread_safe<int> a{ 1 }, b{ 2 }, c{ 3 };
    ts::Task<int> ab = ts::when_all(read_async(a), read_async(b))
                           .then([](std::tuple<int, int>& r) { return std::get<0>(r) + std::get<1>(r); });

    int s = ts::when_all(ab, read_async(c))
                .then([](std::tuple<int, int>& r) { return std::get<0>(r) + std::get<1>(r); })
                .sync();
    TS_CHECK(s == 6);
}

// --- F2: `when_all` completeness (void, move-only, apply-style) ------------

void test_when_all_void_prereq()
{
    ts::Thread_safe<int> a{ 10 }, b{ 32 };
    std::atomic<int> side{ 0 };
    ts::Task<void> v = a.async([&side](int& x) { side.store(x); });   // void prerequisite
    ts::Task<int> r = b.async([](const int& x) { return x; });

    int got = ts::when_all(v, r).then([](std::tuple<int>& t) { return std::get<0>(t); }).sync();
    TS_CHECK(got == 32);            // only the non-void result is carried
    TS_CHECK(side.load() == 10);    // the void prerequisite ran
}

void test_when_all_all_void()
{
    ts::Thread_safe<int> a{ 0 }, b{ 0 };
    std::atomic<int> count{ 0 };
    ts::Task<void> ta = a.async([&count](int&) { count.fetch_add(1); });
    ts::Task<void> tb = b.async([&count](int&) { count.fetch_add(1); });

    ts::when_all(ta, tb).sync();     // all-void join -> Task<void>
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
        .sync();
    TS_CHECK(sum == 12);
}

void test_when_all_apply_style()
{
    ts::Thread_safe<int> a{ 10 }, b{ 32 };
    int sum = ts::when_all(
            a.async([](const int& x) { return x; }),
            b.async([](const int& x) { return x; }))
        .then([](int x, int y) { return x + y; })   // unpacked, not a tuple
        .sync();
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
        .sync();
    TS_CHECK(sink.load() == 3);
}

// --- F3: `when_all` cancellation ------------------------------------------

// A cancelled prerequisite cancels the join (it can't form a complete tuple) instead
// of stalling it.
void test_when_all_cancelled_prereq()
{
    ts::Cancellation_source src;
    src.request_cancel();

    ts::Thread_safe<int> a{ 1 }, b{ 2 };
    ts::Task<int> ta = a.async([](const int& x) { return x; });                // completes
    ts::Task<int> tb = b.async([](const int& x) { return x; }, { .token = src.token() });   // cancelled

    ts::Task<std::tuple<int, int>> j = ts::when_all(ta, tb);
    wait_until([&] { return j.is_done(); });   // settles (does not hang)
    TS_CHECK(j.is_cancelled());
}

// A cancelled void (ordering-only) prerequisite also cancels the join.
void test_when_all_cancelled_void_prereq()
{
    ts::Cancellation_source src;
    src.request_cancel();

    ts::Thread_safe<int> a{ 5 }, b{ 0 };
    ts::Task<int> r = a.async([](const int& x) { return x; });        // completes
    ts::Task<void> v = b.async([](int&) {}, { .token = src.token() });            // cancelled void prereq

    ts::Task<std::tuple<int>> j = ts::when_all(v, r);
    wait_until([&] { return j.is_done(); });
    TS_CHECK(j.is_cancelled());
}

// The cancellation propagates through a `.then` off the join (continuation skipped).
void test_when_all_cancel_propagates_then()
{
    ts::Cancellation_source src;
    src.request_cancel();

    ts::Thread_safe<int> a{ 1 }, b{ 2 };
    ts::Task<int> ta = a.async([](const int& x) { return x; });
    ts::Task<int> tb = b.async([](const int& x) { return x; }, { .token = src.token() });   // cancelled

    std::atomic<bool> then_ran{ false };
    ts::Task<int> j = ts::when_all(ta, tb)
        .then([&then_ran](int x, int y) { then_ran.store(true); return x + y; });   // apply-style
    wait_until([&] { return j.is_done(); });
    TS_CHECK(j.is_cancelled());
    TS_CHECK(!then_ran.load());
}

// --- G: `Signal` ----------------------------------------------------------

void test_signal_trigger_then_wait()
{
    ts::Signal s;
    TS_CHECK(!s.is_done());
    s.trigger();
    s.sync();                    // does not block: already triggered
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

    s.sync();                    // blocks until triggered on the other thread
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
    t.sync();
    TS_CHECK(sink.load() == 42);
}

void test_signal_then_after_trigger()
{
    ts::Signal s;
    s.trigger();
    std::atomic<int> sink{ 0 };
    s.then([&sink] { sink.store(7); }).sync();   // attached after completion: runs inline
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
    s.sync();
    TS_CHECK(fired.load() == 1);
}

void test_signal_copies_share_state()
{
    ts::Signal s;
    ts::Signal copy = s;        // shares one control block
    s.trigger();
    copy.sync();
    TS_CHECK(copy.is_done() && s.is_done());
}

void test_signal_multiple_waiters()
{
    ts::Signal s;
    std::atomic<int> woke{ 0 };
    {
        std::vector<std::jthread> waiters;
        for (int i = 0; i < 8; ++i)
            waiters.emplace_back([&] { s.sync(); woke.fetch_add(1); });   // all block on one signal
        std::this_thread::sleep_for(5ms);   // let them park
        TS_CHECK(woke.load() == 0);
        s.trigger();
    }   // join: every waiter must have woken
    TS_CHECK(woke.load() == 8);
}

// --- H: cancellation ------------------------------------------------------

void test_cancel_before_run()
{
    ts::Cancellation_source src;
    src.request_cancel();               // cancelled before the task is dispatched

    ts::Thread_safe<int> d{ 0 };
    std::atomic<bool> ran{ false };
    ts::Task<int> t = d.async([&ran](int& v) { ran.store(true); return v; }, { .token = src.token() });

    wait_until([&] { return t.is_done(); });
    TS_CHECK(t.is_cancelled());
    TS_CHECK(!ran.load());              // body skipped
}

void test_cancel_propagates_through_then()
{
    ts::Cancellation_source src;
    src.request_cancel();

    ts::Thread_safe<int> d{ 5 };
    ts::Task<int> t = d.async([](const int& v) { return v; }, { .token = src.token() });   // cancelled
    std::atomic<bool> then_ran{ false };
    ts::Task<int> u = t.then([&then_ran](int v) { then_ran.store(true); return v + 1; });

    wait_until([&] { return u.is_done(); });
    TS_CHECK(u.is_cancelled());         // cancellation propagated downstream
    TS_CHECK(!then_ran.load());
}

void test_cancel_at_then_via_token()
{
    ts::Thread_safe<int> d{ 5 };
    ts::Task<int> t = d.async([](const int& v) { return v; });   // completes normally
    wait_until([&] { return t.is_done(); });

    ts::Cancellation_source src;
    src.request_cancel();
    std::atomic<bool> then_ran{ false };
    ts::Task<int> u = t.then([&then_ran](int v) { then_ran.store(true); return v; }, { .token = src.token() });

    wait_until([&] { return u.is_done(); });
    TS_CHECK(u.is_cancelled());         // token cancelled the continuation even though t succeeded
    TS_CHECK(!then_ran.load());
}

void test_cancel_void_get_unblocks()
{
    ts::Cancellation_source src;
    src.request_cancel();

    ts::Thread_safe<int> d{ 0 };
    ts::Task<void> t = d.async([](int& v) { v = 1; }, { .token = src.token() });
    t.sync();                            // void: unblocks, no fatal
    TS_CHECK(t.is_cancelled());
}

void test_not_cancelled_normally()
{
    ts::Thread_safe<int> d{ 7 };
    ts::Task<int> t = d.async([](const int& v) { return v; });
    TS_CHECK(t.sync() == 7);
    TS_CHECK(!t.is_cancelled());
}

void test_death_cancelled_value_get()
{
    TS_CHECK(ts::test::expect_death("cancelled_value_get"));   // get() on a cancelled value task
}

// --- H2: cancel callbacks (push notification) -----------------------------

void test_cancel_callback_fires()
{
    ts::Cancellation_source src;
    std::atomic<int> fired{ 0 };
    ts::Cancel_callback cb(src.token(), [&fired] { fired.fetch_add(1); });
    TS_CHECK(fired.load() == 0);        // not fired before the request
    src.request_cancel();
    TS_CHECK(fired.load() == 1);        // fired synchronously on request
    src.request_cancel();
    TS_CHECK(fired.load() == 1);        // idempotent -> not re-fired
}

void test_cancel_callback_already_requested()
{
    ts::Cancellation_source src;
    src.request_cancel();
    std::atomic<int> fired{ 0 };
    ts::Cancel_callback cb(src.token(), [&fired] { fired.fetch_add(1); });
    TS_CHECK(fired.load() == 1);        // registered after cancel -> fires in the ctor
}

void test_cancel_callback_deregister()
{
    ts::Cancellation_source src;
    std::atomic<int> fired{ 0 };
    {
        ts::Cancel_callback cb(src.token(), [&fired] { fired.fetch_add(1); });
    }   // destroyed before any request
    src.request_cancel();
    TS_CHECK(fired.load() == 0);        // deregistered -> never fires
}

void test_cancel_callback_multiple()
{
    ts::Cancellation_source src;
    std::atomic<int> fired{ 0 };
    ts::Cancel_callback a(src.token(), [&fired] { fired.fetch_add(1); });
    ts::Cancel_callback b(src.token(), [&fired] { fired.fetch_add(1); });
    ts::Cancel_callback c(src.token(), [&fired] { fired.fetch_add(1); });
    src.request_cancel();
    TS_CHECK(fired.load() == 3);        // all fire
}

void test_cancel_callback_stateless_token()
{
    ts::Cancellation_token token;       // default: never cancels
    std::atomic<int> fired{ 0 };
    ts::Cancel_callback cb(token, [&fired] { fired.fetch_add(1); });
    TS_CHECK(fired.load() == 0);        // no source -> no fire, ever
}

// --- I: standalone `launch` (bare scheduler task, body in the block) --------

void test_launch_value()
{
    TS_CHECK(ts::launch([] { return 6 * 7; }).sync() == 42);
}

void test_launch_void()
{
    std::atomic<int> ran{ 0 };
    ts::launch([&ran] { ran.fetch_add(1); }).sync();
    TS_CHECK(ran.load() == 1);
}

void test_launch_then()
{
    int r = ts::launch([] { return 20; }).then([](int v) { return v + 1; }).sync();
    TS_CHECK(r == 21);
}

// `sync()` returns `const R&` and does not consume: the same task can be sync'd repeatedly,
// and `sync()` can coexist with a `then` -- both see the same result.
void test_sync_const_ref_multi()
{
    ts::Task<int> t = ts::launch([] { return 42; });
    const int& a = t.sync();
    const int& b = t.sync();          // second sync -- still valid (non-consuming)
    TS_CHECK(a == 42 && b == 42);
    TS_CHECK(&a == &b);               // same storage: sync() aliased the block's result
    int viathen = t.then([](int v) { return v + 1; }).sync();
    TS_CHECK(viathen == 43);          // then reads the un-consumed result
    TS_CHECK(t.sync() == 42);         // and it is still there after the then
}

// `take()` moves the result out -- works for a move-only `R`, and steals the payload.
void test_take_moves_move_only()
{
    ts::Task<std::unique_ptr<int>> t = ts::launch([] { return std::make_unique<int>(7); });
    std::unique_ptr<int> p = t.take();
    TS_CHECK(p && *p == 7);

    // A moved-out large value: verify ownership actually transferred (source emptied).
    ts::Task<std::vector<int>> v = ts::launch([] { return std::vector<int>{ 1, 2, 3, 4 }; });
    std::vector<int> got = v.take();
    TS_CHECK(got.size() == 4 && got[3] == 4);
}

void test_launch_priority()
{
    // Priority is accepted on every launch route (ordering is covered deterministically by
    // the scheduler + graph tests; here just confirm the API threads through and runs).
    TS_CHECK(ts::launch([] { return 1; }, {}, Priority::high).sync() == 1);
    TS_CHECK(ts::task([] { return 2; }).priority(Priority::low).launch().sync() == 2);

    ts::Thread_safe<int> d{ 40 };
    TS_CHECK(d.async([](const int& v) { return v + 2; }, { .priority = Priority::high }).sync() == 42);
}

// An inline task runs on the thread that settled its last prerequisite. Pinned
// deterministically: the prerequisite blocks on `gate` (so it can't complete until `dep`
// is wired -- otherwise `after` would no-op and `dep` would run on this thread), and we
// wait via a flag, not get() (a get() would retract `dep` onto this thread).
void test_inline_runs_on_completer()
{
    std::atomic<bool> gate{ false };
    std::atomic<std::thread::id> prereq_thread{};
    std::atomic<std::thread::id> inline_thread{};
    std::atomic<bool> inline_ran{ false };

    ts::Task<void> prereq = ts::launch([&]
    {
        while (!gate.load()) std::this_thread::yield();   // held until `dep` is wired
        prereq_thread.store(std::this_thread::get_id());
    });
    ts::Task<void> dep = ts::task([&] { inline_thread.store(std::this_thread::get_id()); inline_ran.store(true); })
                             .set_inline().after(prereq).launch();
    gate.store(true);   // prereq now finishes on its worker; dep dispatches inline there
    wait_until([&] { return inline_ran.load(); });

    TS_CHECK(inline_thread.load() == prereq_thread.load());          // ran on the prerequisite's completer (a worker)
    TS_CHECK(inline_thread.load() != std::this_thread::get_id());    // not this thread
}

// An inline task with no pending prerequisite runs synchronously, on the launching thread.
void test_inline_synchronous_when_ready()
{
    std::atomic<bool> ran{ false };
    ts::task([&] { ran.store(true); }).set_inline().launch();
    TS_CHECK(ran.load());   // already ran before this line
}

// A long chain of inline tasks runs iteratively (the trampoline), not recursively -- so it
// does not overflow the stack. Held off a Signal so the whole chain is built before it fires.
void test_inline_deep_chain_no_overflow()
{
    constexpr int n = 20000;
    std::atomic<int> count{ 0 };

    ts::Signal root;
    ts::Task<void> prev = root;
    for (int i = 0; i < n; ++i)
        prev = ts::task([&count] { count.fetch_add(1, std::memory_order_relaxed); })
                   .set_inline().after(prev).launch();

    root.trigger();    // fires the chain -> all n run inline on this thread, trampolined
    prev.sync();
    TS_CHECK(count.load() == n);
}

// A task body that declares a trailing `Cancellation_token` receives the task's token and
// can poll it to early-out. Cancellation arrives WHILE the body runs (the pre-run skip
// does not apply -- the task already started), so this exercises the body parameter, not
// the dispatch-time skip. A cooperative early-out returns normally, so the task COMPLETES.
void test_task_body_token_earlyout()
{
    ts::Cancellation_source src;
    std::atomic<bool> started{ false };
    std::atomic<int> stage{ 0 };

    ts::Task<void> t = ts::launch([&](ts::Cancellation_token tok)
    {
        started.store(true);
        while (!tok.is_cancel_requested())
            std::this_thread::yield();   // running -- poll the token
        stage.store(1);                  // observed cancellation mid-body -> early out
    }, src.token());

    wait_until([&] { return started.load(); });   // body started before we cancel
    src.request_cancel();
    t.sync();
    TS_CHECK(stage.load() == 1);          // the body received and reacted to the token
    TS_CHECK(!t.is_cancelled());          // cooperative return -> COMPLETED, not cancelled
}

// The builder path also forwards the token to a token-taking body.
void test_task_builder_token_earlyout()
{
    ts::Cancellation_source src;
    std::atomic<bool> started{ false };
    std::atomic<int> stage{ 0 };

    ts::Task<int> t = ts::task([&](ts::Cancellation_token tok) -> int
    {
        started.store(true);
        while (!tok.is_cancel_requested())
            std::this_thread::yield();
        stage.store(1);
        return 42;
    }).launch(src.token());

    wait_until([&] { return started.load(); });
    src.request_cancel();
    TS_CHECK(t.sync() == 42);
    TS_CHECK(stage.load() == 1);
}

// An async read accessor may take a trailing token and early-out mid-run.
void test_async_body_token_earlyout()
{
    ts::Cancellation_source src;
    std::atomic<bool> started{ false };
    std::atomic<int> stage{ 0 };

    ts::Thread_safe<int> d{ 5 };
    ts::Task<int> t = d.async([&started, &stage](const int& v, ts::Cancellation_token tok) -> int
    {
        started.store(true);
        while (!tok.is_cancel_requested())
            std::this_thread::yield();
        stage.store(1);
        return v;
    }, { .token = src.token() });

    wait_until([&] { return started.load(); });
    src.request_cancel();
    TS_CHECK(t.sync() == 5);          // early-outed, completed with the value
    TS_CHECK(stage.load() == 1);
}

// A write accessor (T&) with a trailing token is still deduced read_write.
void test_async_write_token()
{
    ts::Thread_safe<int> d{ 0 };
    d.async([](int& v, ts::Cancellation_token) { v = 9; }).sync();   // mutates -> write path
    TS_CHECK(d.async([](const int& v) { return v; }).sync() == 9);
}

// `then` accepts dispatch options (priority, inline, token); both flavors run and return.
void test_then_options()
{
    ts::Task<int> a = ts::launch([] { return 10; });
    TS_CHECK(a.then([](int v) { return v + 1; }, { .priority = Priority::high }).sync() == 11);

    ts::Task<int> c = ts::launch([] { return 20; });
    TS_CHECK(c.then([](int v) { return v + 2; }, { .run_inline = true }).sync() == 22);
}

// An inline `then` runs on the thread that completes the producer (same mechanism as
// Task_builder::set_inline). Pinned deterministically like test_inline_runs_on_completer.
void test_then_inline_on_completer()
{
    std::atomic<bool> gate{ false };
    std::atomic<std::thread::id> producer_thread{};
    std::atomic<std::thread::id> then_thread{};
    std::atomic<bool> then_ran{ false };

    ts::Task<int> producer = ts::launch([&]
    {
        while (!gate.load()) std::this_thread::yield();   // held until the continuation is wired
        producer_thread.store(std::this_thread::get_id());
        return 5;
    });
    ts::Task<int> cont = producer.then([&](int v)
    {
        then_thread.store(std::this_thread::get_id());
        then_ran.store(true);
        return v;
    }, { .run_inline = true });
    gate.store(true);
    wait_until([&] { return then_ran.load(); });

    TS_CHECK(then_thread.load() == producer_thread.load());       // ran on the producer's completer
    TS_CHECK(then_thread.load() != std::this_thread::get_id());   // not this thread
    (void)cont;
}

// A then continuation body (value shape) may take a trailing token and early-out mid-run;
// it forwards the continuation's OWN token (`opts.token`). A cooperative return completes.
void test_then_body_token_earlyout()
{
    ts::Cancellation_source src;
    std::atomic<bool> started{ false };
    std::atomic<int> stage{ 0 };

    ts::Task<int> p = ts::launch([] { return 3; });
    ts::Task<int> u = p.then([&started, &stage](int v, ts::Cancellation_token tok) -> int
    {
        started.store(true);
        while (!tok.is_cancel_requested())
            std::this_thread::yield();
        stage.store(1);
        return v + 1;
    }, { .token = src.token() });

    wait_until([&] { return started.load(); });
    src.request_cancel();
    TS_CHECK(u.sync() == 4);
    TS_CHECK(stage.load() == 1);
    TS_CHECK(!u.is_cancelled());
}

// Void-producer and apply-style continuations also accept a trailing token.
void test_then_body_token_shapes()
{
    // void producer
    ts::Cancellation_source src;
    std::atomic<bool> started{ false };
    std::atomic<int> stage{ 0 };
    ts::Task<void> p = ts::launch([] {});
    ts::Task<void> u = p.then([&started, &stage](ts::Cancellation_token tok)
    {
        started.store(true);
        while (!tok.is_cancel_requested())
            std::this_thread::yield();
        stage.store(1);
    }, { .token = src.token() });
    wait_until([&] { return started.load(); });
    src.request_cancel();
    u.sync();
    TS_CHECK(stage.load() == 1);

    // apply-style + token (not cancelled: just exercises the branch's type deduction + run)
    ts::Thread_safe<int> a{ 2 }, b{ 3 };
    ts::Task<int> ra = a.async([](const int& v) { return v; });
    ts::Task<int> rb = b.async([](const int& v) { return v; });
    int s = ts::when_all(ra, rb)
                .then([](int x, int y, ts::Cancellation_token tok) { (void)tok; return x + y; })
                .sync();
    TS_CHECK(s == 5);
}

void test_launch_cancelled()
{
    ts::Cancellation_source src;
    src.request_cancel();
    std::atomic<bool> ran{ false };
    ts::Task<int> t = ts::launch([&ran] { ran.store(true); return 1; }, src.token());
    wait_until([&] { return t.is_done(); });
    TS_CHECK(t.is_cancelled());
    TS_CHECK(!ran.load());
}

// --- J: prerequisites (ts::task(...).after(...).launch()) ------------------

void test_task_after_single()
{
    std::atomic<int> order{ 0 };
    std::atomic<int> a_order{ 0 }, b_order{ 0 };
    ts::Task<void> a = ts::launch([&] { a_order.store(++order); });
    ts::Task<void> b = ts::task([&] { b_order.store(++order); }).after(a).launch();
    b.sync();
    TS_CHECK(a_order.load() == 1 && b_order.load() == 2);   // a ran before b
}

void test_task_after_multiple()
{
    std::atomic<int> prereqs_done{ 0 };
    std::atomic<int> seen{ -1 };
    ts::Task<void> a = ts::launch([&] { std::this_thread::sleep_for(3ms); prereqs_done.fetch_add(1); });
    ts::Task<void> b = ts::launch([&] { std::this_thread::sleep_for(3ms); prereqs_done.fetch_add(1); });
    ts::task([&] { seen.store(prereqs_done.load()); }).after(a, b).launch().sync();
    TS_CHECK(seen.load() == 2);   // ran only after BOTH prerequisites settled
}

void test_task_value_after()
{
    ts::Task<int> a = ts::launch([] { return 10; });
    ts::Task<int> b = ts::task([] { return 32; }).after(a).launch();
    TS_CHECK(a.sync() + b.sync() == 42);
}

void test_task_after_already_completed()
{
    ts::Task<int> a = ts::launch([] { return 5; });
    a.sync();   // a already settled
    ts::Task<int> b = ts::task([] { return 7; }).after(a).launch();
    TS_CHECK(b.sync() == 7);   // still runs
}

void test_task_after_cancelled_prereq()
{
    ts::Cancellation_source src;
    src.request_cancel();
    ts::Task<void> a = ts::launch([] {}, src.token());   // cancelled prerequisite
    std::atomic<bool> ran{ false };
    ts::Task<void> b = ts::task([&] { ran.store(true); }).after(a).launch();
    b.sync();   // void get on a cancelled task unblocks
    TS_CHECK(a.is_cancelled());
    TS_CHECK(b.is_cancelled());       // cancellation propagates through `after` (like `then`)
    TS_CHECK(!ran.load());            // the dependent's body did not run
}

// Multi-prerequisite: one cancelled prerequisite (of several) cancels the dependent.
void test_task_after_cancel_propagates_multi()
{
    ts::Cancellation_source src;
    src.request_cancel();
    ts::Task<void> a = ts::launch([] {});                    // completes
    ts::Task<void> b = ts::launch([] {}, src.token());       // cancelled
    ts::Task<void> c = ts::launch([] {});                    // completes
    std::atomic<bool> ran{ false };
    ts::Task<void> dep = ts::task([&] { ran.store(true); }).after(a, b, c).launch();
    dep.sync();
    TS_CHECK(dep.is_cancelled());     // any cancelled prerequisite propagates
    TS_CHECK(!ran.load());
}

// --- J2: reusable tasks (reset) --------------------------------------------

// The builder is the reusable handle: one block/body, re-run across runs via reset().
void test_reuse_value()
{
    std::atomic<int> counter{ 0 };
    auto t = ts::task([&counter] { return counter.fetch_add(1) + 1; });

    t.launch();
    TS_CHECK(t.sync() == 1);
    t.reset().launch();
    TS_CHECK(t.sync() == 2);
    t.reset().launch();
    TS_CHECK(t.sync() == 3);   // fresh result each run, same block
}

void test_reuse_void()
{
    std::atomic<int> runs{ 0 };
    auto t = ts::task([&runs] { runs.fetch_add(1); });

    t.launch();
    t.sync();
    for (int i = 0; i < 4; ++i)
    {
        t.reset().launch();
        t.sync();
    }
    TS_CHECK(runs.load() == 5);
}

// Prerequisites are re-established each run.
void test_reuse_prereq()
{
    std::atomic<int> log{ 0 };
    auto dependent = ts::task([&log] { return log.load(); });

    for (int run = 1; run <= 3; ++run)
    {
        if (run > 1)
            dependent.reset();
        ts::Task<void> prereq = ts::launch([&log, run] { log.store(run * 10); });
        dependent.after(prereq).launch();
        TS_CHECK(dependent.sync() == run * 10);   // ran after this run's fresh prerequisite
    }
}

// A Signal re-armed via reset() acts as a reusable phase gate.
void test_signal_reset()
{
    ts::Signal sig;
    sig.trigger();
    sig.sync();
    TS_CHECK(sig.is_done());

    sig.reset();
    TS_CHECK(!sig.is_done());   // re-armed
    sig.trigger();
    sig.sync();
    TS_CHECK(sig.is_done());
}

void test_death_reset_unsettled()
{
    TS_CHECK(ts::test::expect_death("reset_unsettled"));   // reset() before the task settled
}

// --- K: nested tasks -------------------------------------------------------

// A nested task launched inside a parent's body gates the parent's completion.
void test_nested_gates_parent()
{
    std::atomic<int> nested_done{ 0 };
    ts::Task<void> parent = ts::launch([&]
    {
        ts::nested([&] { std::this_thread::sleep_for(10ms); nested_done.fetch_add(1); });
        // parent body returns now, but the task must not complete until nested does
    });
    parent.sync();
    TS_CHECK(nested_done.load() == 1);   // nested finished before the parent completed
}

void test_nested_multiple()
{
    std::atomic<int> count{ 0 };
    ts::launch([&]
    {
        for (int i = 0; i < 3; ++i)
            ts::nested([&] { std::this_thread::sleep_for(5ms); count.fetch_add(1); });
    }).sync();
    TS_CHECK(count.load() == 3);   // all nested tasks done before the parent completed
}

void test_add_nested_existing()
{
    std::atomic<bool> done{ false };
    ts::launch([&]
    {
        ts::Task<void> child = ts::launch([&] { std::this_thread::sleep_for(10ms); done.store(true); });
        ts::add_nested(child);   // nest an already-launched task
    }).sync();
    TS_CHECK(done.load());
}

// The parent's continuation fires only after the parent completes — i.e. after nested.
void test_nested_then_after()
{
    std::atomic<int> order{ 0 };
    std::atomic<int> nested_order{ 0 }, then_order{ 0 };
    ts::launch([&]
    {
        ts::nested([&] { std::this_thread::sleep_for(10ms); nested_order.store(++order); });
    }).then([&] { then_order.store(++order); }).sync();
    TS_CHECK(nested_order.load() == 1 && then_order.load() == 2);
}

void test_death_add_nested_outside()
{
    TS_CHECK(ts::test::expect_death("add_nested_outside"));   // no running task
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
    run("when_all cancelled prereq", test_when_all_cancelled_prereq);
    run("when_all cancelled void prereq", test_when_all_cancelled_void_prereq);
    run("when_all cancel propagates then", test_when_all_cancel_propagates_then);
    run("signal trigger then wait", test_signal_trigger_then_wait);
    run("signal wait then trigger", test_signal_wait_then_trigger);
    run("signal then", test_signal_then);
    run("signal then after trigger", test_signal_then_after_trigger);
    run("signal idempotent trigger", test_signal_idempotent_trigger);
    run("signal copies share state", test_signal_copies_share_state);
    run("signal multiple waiters", test_signal_multiple_waiters);
    run("cancel before run", test_cancel_before_run);
    run("cancel propagates through then", test_cancel_propagates_through_then);
    run("cancel at then via token", test_cancel_at_then_via_token);
    run("cancel void get unblocks", test_cancel_void_get_unblocks);
    run("not cancelled normally", test_not_cancelled_normally);
    run("death: cancelled value get", test_death_cancelled_value_get);
    run("cancel callback fires", test_cancel_callback_fires);
    run("cancel callback already requested", test_cancel_callback_already_requested);
    run("cancel callback deregister", test_cancel_callback_deregister);
    run("cancel callback multiple", test_cancel_callback_multiple);
    run("cancel callback stateless token", test_cancel_callback_stateless_token);
    run("launch value", test_launch_value);
    run("launch void", test_launch_void);
    run("launch then", test_launch_then);
    run("sync const-ref multi-consumer", test_sync_const_ref_multi);
    run("take moves move-only", test_take_moves_move_only);
    run("launch priority", test_launch_priority);
    run("inline runs on completer", test_inline_runs_on_completer);
    run("inline synchronous when ready", test_inline_synchronous_when_ready);
    run("inline deep chain no overflow", test_inline_deep_chain_no_overflow);
    run("task body token earlyout", test_task_body_token_earlyout);
    run("task builder token earlyout", test_task_builder_token_earlyout);
    run("async body token earlyout", test_async_body_token_earlyout);
    run("async write token", test_async_write_token);
    run("then options", test_then_options);
    run("then inline on completer", test_then_inline_on_completer);
    run("then body token earlyout", test_then_body_token_earlyout);
    run("then body token shapes", test_then_body_token_shapes);
    run("launch cancelled", test_launch_cancelled);
    run("task after single", test_task_after_single);
    run("task after multiple", test_task_after_multiple);
    run("task value after", test_task_value_after);
    run("task after already completed", test_task_after_already_completed);
    run("task after cancelled prereq", test_task_after_cancelled_prereq);
    run("task after cancel propagates multi", test_task_after_cancel_propagates_multi);
    run("reuse value task", test_reuse_value);
    run("reuse void task", test_reuse_void);
    run("reuse task with prereq", test_reuse_prereq);
    run("signal reset", test_signal_reset);
    run("death: reset unsettled", test_death_reset_unsettled);
    run("nested gates parent", test_nested_gates_parent);
    run("nested multiple", test_nested_multiple);
    run("add nested existing", test_add_nested_existing);
    run("nested then after", test_nested_then_after);
    run("death: add_nested outside", test_death_add_nested_outside);
}
