#include "coroutine_tests.h"
#include "harness.h"

#if defined(__cpp_impl_coroutine)

#include "coroutine_support.h"
#include "thread_safe.h"
#include "test_util.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <tuple>
#include <utility>

using ts::test::run;
using ts::Task;
using ts::Cancellation_source;
using ts::Cancellation_token;

namespace
{

// 1. Simplest coroutine: `co_return` a value, consumed via the ordinary `sync()`.
Task<int> co_answer()
{
    co_return 42;
}

void test_simple()
{
    TS_CHECK(co_answer().sync() == 42);
}

// 2. Chained: `co_await` several tasks, use their results as locals. Linear where the
// callback form would nest.
Task<int> co_chained()
{
    int a = co_await ts::launch([] { return 10; });
    int b = co_await ts::launch([] { return 20; });
    co_return a + b + 12;
}

void test_chained()
{
    TS_CHECK(co_chained().sync() == 42);
}

// 3. The killer example: read a `Thread_safe` (const accessor), compute, then write it
// (mutable accessor). No access is held across the `co_await` -- each `async` acquires and
// releases its own pipe -- so it is the *safe* shape.
Task<void> co_thread_safe(ts::Thread_safe<int>& obj, int* seen_out)
{
    int seen = co_await obj.async([](const int& v) { return v; });        // read
    co_await obj.async([seen](int& v) { v += seen + 1; });                // write
    *seen_out = seen;
}

void test_thread_safe()
{
    ts::Thread_safe<int> obj{ 5 };
    int seen = -1;
    co_thread_safe(obj, &seen).sync();
    TS_CHECK(seen == 5);
    TS_CHECK(obj.async([](const int& v) { return v; }).sync() == 11);   // 5 + (5 + 1)
}

// 4. Fan-out / fan-in: start two tasks, then `co_await when_all`, unpacking the tuple.
Task<int> co_when_all()
{
    Task<int> t1 = ts::launch([] { return 3; });
    Task<int> t2 = ts::launch([] { return 4; });
    auto [a, b] = co_await ts::when_all(std::move(t1), std::move(t2));
    co_return a * b;
}

void test_when_all()
{
    TS_CHECK(co_when_all().sync() == 12);
}

// 5. A `co_await` loop -- the case continuations cannot express without recursion.
Task<int> co_loop()
{
    int m = 1;
    for (int i = 0; i < 5; ++i)
        m = co_await ts::launch([m] { return m * 2; });
    co_return m;   // 2^5 == 32
}

void test_loop()
{
    TS_CHECK(co_loop().sync() == 32);
}

// 6. Cancellation as ordinary control flow (no exceptions): observe a cancelled prerequisite
// via `is_cancelled()` and early-out with a value.
Task<int> co_cancel(Cancellation_token tok)
{
    Task<int> t = ts::launch([] { return 7; }, tok);
    while (!t.is_done())
        std::this_thread::yield();
    if (t.is_cancelled())
        co_return -1;
    co_return co_await t;
}

void test_cancel()
{
    Cancellation_source src;
    src.request_cancel();
    TS_CHECK(co_cancel(src.token()).sync() == -1);
}

// 7. Access-context threading: a coroutine created under a grant re-installs it on every
// resumed segment, so a body touching guarded data AFTER a suspension does not fault the
// harness. The `Signal` gate makes the suspension + resume deterministic and, crucially,
// resumes OUTSIDE the original `Access_scope` (below) -- so `current_access` is null at resume
// and only the promise's re-installed snapshot lets `increment()`/`value()` pass.
Task<int> co_touch_after_await(tests::Counter& c, ts::Signal& gate)
{
    co_await gate;        // suspends until triggered
    c.increment();        // guarded -- faults the harness without the re-installed grant
    co_return c.value();
}

void test_access_context()
{
    tests::Counter c;
    ts::Signal gate;
    ts::Access_context ctx;
    ctx.add(&c, ts::Access::read_write);

    Task<int> t;
    {
        ts::Access_scope scope(ctx);
        t = co_touch_after_await(c, gate);   // snapshots the grant; suspends on `gate`
    }   // grant scope ends -- the coroutine keeps its snapshot copy

    gate.trigger();               // resume runs here (no ambient grant) under the re-installed one
    TS_CHECK(t.sync() == 1);
}

} // namespace

void run_coroutine_tests()
{
    std::printf("\n[coroutine] tests\n");
    run("co simple", test_simple);
    run("co chained", test_chained);
    run("co thread_safe", test_thread_safe);
    run("co when_all", test_when_all);
    run("co loop", test_loop);
    run("co cancel", test_cancel);
    run("co access context", test_access_context);
}

#else   // no coroutine support in this toolchain

void run_coroutine_tests() {}

#endif
