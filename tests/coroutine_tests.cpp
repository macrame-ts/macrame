#include "coroutine_tests.h"
#include "harness.h"

#if defined(__cpp_impl_coroutine)

#include "coroutine_support.h"
#include "guarded.h"
#include "parallel_for.h"
#include "test_util.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

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

// 3. The killer example: read a `Guarded` (const accessor), compute, then write it
// (mutable accessor). No access is held across the `co_await` -- each `async` acquires and
// releases its own pipe -- so it is the *safe* shape.
Task<void> co_thread_safe(ts::Guarded<int>& obj, int* seen_out)
{
    int seen = co_await obj.async([](const int& v) { return v; });        // read
    co_await obj.async([seen](int& v) { v += seen + 1; });                // write
    *seen_out = seen;
}

void test_thread_safe()
{
    ts::Guarded<int> obj{ 5 };
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
    Task<int> t = ts::launch([] { return 7; }, { .token = tok });
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

// 8. The async-lock guard: `co_await ts::read_write(w)` acquires the pipe and resumes with an RAII
// guard giving direct `Counter&`; mutate it, release at scope end, then read it back via a read
// guard. Straight-line RAII in place of a callback `async`.
Task<int> co_write_guard(ts::Guarded<tests::Counter>& w)
{
    {
        auto g = co_await ts::read_write(w);   // exclusive; guard grants `Counter&`
        g->increment();
        g->add(5);
    }   // guard released -- pipe free again
    auto r = co_await ts::read_only(w);        // shared reader; `const Counter&`
    co_return r->value();
}

void test_write_guard()
{
    ts::Guarded<tests::Counter> w;
    TS_CHECK(co_write_guard(w).sync() == 6);   // 1 + 5
}

// 9. Read guard: shared access, `const` view -- the harness passes for a const method inside it.
Task<int> co_read_guard(ts::Guarded<tests::Counter>& w)
{
    auto g = co_await ts::read_only(w);
    co_return g->value();
}

void test_read_guard()
{
    ts::Guarded<tests::Counter> w;
    w.async([](tests::Counter& c) { c.add(9); }).sync();
    TS_CHECK(co_read_guard(w).sync() == 9);
}

// 10. Guard + control flow: a loop inside ONE write-guard scope -- the ergonomic win over an
// `async` lambda (no `co_await` in the loop, so the pipe is held for the whole critical section).
Task<int> co_guard_loop(ts::Guarded<tests::Counter>& w, int n)
{
    auto g = co_await ts::read_write(w);
    for (int i = 0; i < n; ++i)
        g->increment();
    co_return g->value();
}

void test_guard_loop()
{
    ts::Guarded<tests::Counter> w;
    TS_CHECK(co_guard_loop(w, 10).sync() == 10);
}

// 11. Contention: many threads each drive a coroutine that repeatedly acquires the write guard
// on the SAME object. The pipe serializes the writers (deferred acquire -> suspend -> resume on
// the releasing thread), so the total is exact. The concurrency test.
Task<void> co_bump(ts::Guarded<tests::Counter>& w, int times)
{
    for (int i = 0; i < times; ++i)
    {
        auto g = co_await ts::read_write(w);   // may defer + resume cross-thread under contention
        g->increment();
    }   // released each iteration -- no guard held across the next co_await
}

void test_guard_contention()
{
    ts::Guarded<tests::Counter> w;
    constexpr int threads = 8, each = 200;
    {
        std::vector<std::jthread> drivers;
        for (int i = 0; i < threads; ++i)
            drivers.emplace_back([&w] { co_bump(w, each).sync(); });
    }   // join
    TS_CHECK(co_read_guard(w).sync() == threads * each);
}

// 12. The suspension detector: `co_await` other work while holding a guard faults. Subprocess
// death test (the fatal aborts) -- the scenario lives in `run_death_scenario` (tests.cpp).
void test_death_await_under_guard()
{
    TS_CHECK(ts::test::expect_death("coro_await_under_guard"));
}

// 13. Feature showcase: one coroutine weaving the whole system into straight-line code --
// prioritized producers joined by when_all (dependencies), a task that forks nested work,
// a Guarded write-guard critical section, async_parallel_for, a Signal phase gate,
// cooperative cancellation, and a final async read.
Task<int> co_showcase(ts::Guarded<tests::Counter>& world, ts::Signal& phase, Cancellation_token tok)
{
    // (a) priority + dependency fan-in: a high- and a low-priority producer, joined.
    Task<int> hi = ts::launch([] { return 3; }, { .priority = ts::Priority::high });
    Task<int> lo = ts::launch([] { return 4; }, { .priority = ts::Priority::low });
    auto [a, b] = co_await ts::when_all(std::move(hi), std::move(lo));      // 3, 4

    // (b) nested tasks: a task body forks nested work; its completion gates on them.
    // `nested_sum` lives in the coroutine frame, so it outlives the forked nested tasks.
    std::atomic<int> nested_sum{ 0 };
    co_await ts::launch([&nested_sum]
    {
        for (int k = 0; k < 4; ++k)
            ts::nested([&nested_sum, k] { nested_sum.fetch_add(k, std::memory_order_relaxed); });
    });                                                                     // nested_sum == 6

    // (c) Guarded write-guard: a critical section over the object, in place.
    {
        auto g = co_await ts::read_write(world);
        g->add(a + b);                                                     // +7
        g->add(nested_sum.load(std::memory_order_relaxed));               // +6  -> world == 13
    }   // pipe released

    // (d) data-parallel fan-out, awaited.
    std::atomic<int> pf{ 0 };
    co_await ts::async_parallel_for(100, [&pf](int) { pf.fetch_add(1, std::memory_order_relaxed); });

    // (e) phase gate: block on an external Signal.
    co_await phase;

    // (f) cooperative cancellation as ordinary control flow.
    if (tok.is_cancel_requested())
        co_return -1;

    // (g) final read via async.
    int total = co_await world.async([](const tests::Counter& c) { return c.value(); });   // 13
    co_return total + pf.load(std::memory_order_relaxed);                  // 13 + 100 == 113
}

void test_showcase()
{
    // Full path: everything runs, phase released, not cancelled.
    {
        ts::Guarded<tests::Counter> world;
        ts::Signal phase;
        ts::Cancellation_source src;
        Task<int> t = co_showcase(world, phase, src.token());
        phase.trigger();
        TS_CHECK(t.sync() == 113);
    }
    // Cancelled path: same pipeline, early-out after the phase gate.
    {
        ts::Guarded<tests::Counter> world;
        ts::Signal phase;
        ts::Cancellation_source src;
        src.request_cancel();
        Task<int> t = co_showcase(world, phase, src.token());
        phase.trigger();
        TS_CHECK(t.sync() == -1);
    }
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
    run("co write guard", test_write_guard);
    run("co read guard", test_read_guard);
    run("co guard loop", test_guard_loop);
    run("co guard contention", test_guard_contention);
    run("co await-under-guard fatal", test_death_await_under_guard);
    run("co showcase", test_showcase);
}

#else   // no coroutine support in this toolchain

void run_coroutine_tests() {}

#endif
