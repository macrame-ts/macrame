#include "coroutine_tests.h"
#include "harness.h"

#if defined(__cpp_impl_coroutine)

#include "ts/coroutine_support.h"
#include "ts/guarded.h"
#include "ts/parallel_for.h"
#include "ts/static_task_graph.h"
#include "ts/task_scope.h"
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

// 4. Fan-out / fan-in: start two tasks (they run eagerly, concurrently), then await both --
// sequential awaits complete when the last one does.
Task<int> co_join()
{
    Task<int> t1 = ts::launch([] { return 3; });
    Task<int> t2 = ts::launch([] { return 4; });
    co_return co_await t1 * co_await t2;
}

void test_join()
{
    TS_CHECK(co_join().sync() == 12);
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
// prioritized producers awaited as a join (dependencies), a task that forks nested work,
// a Guarded write-guard critical section, async_parallel_for, a Signal phase gate,
// cooperative cancellation, and a final async read.
Task<int> co_showcase(ts::Guarded<tests::Counter>& world, ts::Signal& phase, Cancellation_token tok)
{
    // (a) priority + dependency fan-in: a high- and a low-priority producer, both awaited.
    Task<int> hi = ts::launch([] { return 3; }, { .priority = ts::Priority::high });
    Task<int> lo = ts::launch([] { return 4; }, { .priority = ts::Priority::low });
    int a = co_await hi;                                                    // 3
    int b = co_await lo;                                                    // 4

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

// --- coroutine-first §6 matrix companions ---------------------------------

// Companion to the `sync_in_task` fatal: the sanctioned wait inside a task is `co_await` --
// suspend (freeing the worker) until the access's turn, no park, no fatal. A blocker holds
// the pipe first so the await genuinely suspends.
Task<int> await_instead_of_sync(ts::Guarded<int>& b, std::atomic<bool>& release)
{
    ts::Task<void> blocker = b.async([&release](int&)
    {
        while (!release.load(std::memory_order_relaxed))
            std::this_thread::yield();
    });
    ts::Task<int> write = b.async([](int& v) { v = 7; return v; });
    release.store(true, std::memory_order_relaxed);
    int v = co_await write;   // suspends behind the blocker, resumes when granted
    co_return v;
}

void test_coro_await_instead_of_sync()
{
    ts::Guarded<int> b{ 0 };
    std::atomic<bool> release{ false };
    TS_CHECK(await_instead_of_sync(b, release).sync() == 7);
}

// Companion to the `await_cancelled_value` fatal: check `is_cancelled()` first, then branch --
// the same precondition discipline as `sync()`.
Task<int> await_cancelled_checked(ts::Task<int> maybe_cancelled)
{
    while (!maybe_cancelled.is_done())
        std::this_thread::yield();
    if (maybe_cancelled.is_cancelled())
        co_return -1;             // handled: no await of a result that does not exist
    co_return co_await maybe_cancelled;
}

void test_await_cancelled_checked()
{
    ts::Cancellation_source src;
    src.request_cancel();
    ts::Guarded<int> d{ 0 };
    ts::Task<int> t = d.async([](const int& v) { return v; }, { .token = src.token() });
    TS_CHECK(await_cancelled_checked(std::move(t)).sync() == -1);
}

#if TS_SAFETY_CHECKS
void test_death_await_cancelled_value()
{
    TS_CHECK(ts::test::expect_death("await_cancelled_value"));
}
#endif

// The reentrant access arm (coroutine-first §4.2, doctrine (b)): `access` from a task that
// already holds the object's write grant runs inline under it instead of queueing behind
// itself (which used to be the sharp same-object deadlock).
void test_access_reentrant_under_own_grant()
{
    ts::Guarded<int> a{ 0 };
    ts::Static_task_graph g;
    std::atomic<int> seen{ -1 };
    g.add_node([&a, &seen](int& v)
    {
        v = 5;
        ts::Task<int> r = a.access([](const int& x) { return x; });   // reentrant: inline, done
        TS_CHECK(r.is_done());
        seen.store(r.sync());   // settled -> sync is a plain read, legal in-task
    }, a);
    g.compile();
    g.execute().sync();
    TS_CHECK(seen.load() == 5);
}

// --- stage 2: implicit scope, Task_scope, coroutine nodes -----------------

// The §4.4 shape: a coroutine node body -- nested fan-out joins the node's implicit scope,
// a mid-body join makes the results usable, a foreign read awaits under held grants, and
// the node completes (releasing grants, unlocking successors) only at frame completion.
void test_coroutine_graph_node()
{
    ts::Guarded<std::vector<int>> phys{ std::vector<int>{ 1, 2, 3 } };
    ts::Guarded<int> audio{ 40 };
    ts::Guarded<int> result{ 0 };
    std::atomic<int> total{ 0 };
    std::atomic<int> successor_runs{ 0 };

    ts::Static_task_graph g;
    g.add_node([&audio, &total](const std::vector<int>& islands, int& out) -> ts::Task<void>
    {
        total.store(0);                                             // re-run-safe
        for (int island : islands)                                  // data-dependent fan-out
            ts::nested([&total, island] { total.fetch_add(island); });

        co_await ts::join_nested();                                 // solves needed mid-body
        TS_CHECK(total.load() == 6);

        int mix = co_await audio.access([](const int& a) { return a; });   // foreign read (c)
        out = total.load() + mix;
        co_return;
    }, phys, result);
    g.add_node([&successor_runs](const int& out)
    {
        TS_CHECK(out == 46);   // the successor sees the frame's full effect (post-join, post-await)
        successor_runs.fetch_add(1);
    }, result);
    g.compile();
    g.execute().sync();
    TS_CHECK(successor_runs.load() == 1);

    g.execute().sync();   // re-run: the node re-arms; the frame is per-run
    TS_CHECK(successor_runs.load() == 2);
}

// join_nested resets the list: children launched after the join gate co_return as usual.
Task<int> two_phase_nested(std::atomic<int>& counter)
{
    ts::nested([&counter] { counter.fetch_add(1); });
    co_await ts::join_nested();
    int after_first = counter.load();
    ts::nested([&counter] { counter.fetch_add(10); });   // gates completion via the counter
    co_return after_first;
}

void test_join_nested_two_phase()
{
    std::atomic<int> counter{ 0 };
    ts::Task<int> t = two_phase_nested(counter);
    TS_CHECK(t.sync() == 1);          // the join saw phase one...
    TS_CHECK(counter.load() == 11);   // ...and completion gated on phase two
}

// Explicit Task_scope: launch several, join once; the handle stays usable individually.
Task<int> scoped_fanout()
{
    ts::Task_scope scope;
    std::atomic<int>* sum = new std::atomic<int>{ 0 };
    for (int i = 1; i <= 4; ++i)
        scope.launch([sum, i] { sum->fetch_add(i); });
    co_await scope.join();
    int v = sum->load();
    delete sum;
    co_return v;
}

void test_task_scope_join()
{
    TS_CHECK(scoped_fanout().sync() == 10);
}

#if TS_SAFETY_CHECKS
// Companion pair for the lost-children fatal: joining before scope exit is the sanctioned
// form (above); dropping a scope with recorded children is fatal.
void test_death_scope_unjoined()
{
    TS_CHECK(ts::test::expect_death("scope_unjoined"));
}
#endif

} // namespace

void run_coroutine_tests()
{
    std::printf("\n[coroutine] tests\n");
    run("co simple", test_simple);
    run("co chained", test_chained);
    run("co thread_safe", test_thread_safe);
    run("co join", test_join);
    run("co loop", test_loop);
    run("co cancel", test_cancel);
    run("co access context", test_access_context);
    run("co write guard", test_write_guard);
    run("co read guard", test_read_guard);
    run("co guard loop", test_guard_loop);
    run("co guard contention", test_guard_contention);
    run("co await-under-guard fatal", test_death_await_under_guard);
    run("co await instead of sync", test_coro_await_instead_of_sync);
    run("co await cancelled checked", test_await_cancelled_checked);
#if TS_SAFETY_CHECKS
    run("death: await cancelled value", test_death_await_cancelled_value);
#endif
    run("co access reentrant under own grant", test_access_reentrant_under_own_grant);
    run("co graph node", test_coroutine_graph_node);
    run("co join_nested two-phase", test_join_nested_two_phase);
    run("co task_scope join", test_task_scope_join);
#if TS_SAFETY_CHECKS
    run("death: task_scope unjoined", test_death_scope_unjoined);
#endif
    run("co showcase", test_showcase);
}

#else   // no coroutine support in this toolchain

void run_coroutine_tests() {}

#endif
