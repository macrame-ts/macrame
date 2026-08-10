#include "coroutine_tests.h"
#include "harness.h"

#include "ts/coroutine_support.h"
#include "ts/guarded.h"
#include "ts/parallel_for.h"
#include "ts/scheduler.h"
#include "ts/static_task_graph.h"
#include "ts/frame_gate.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using ts::test::run;
using namespace ts::test;
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

// 3. Read a `Guarded` (const accessor), compute, then write it
// (mutable accessor). No access is held across the `co_await` - each `async` acquires and
// releases its own pipe - so it is the *safe* shape.
Task<void> co_thread_safe(ts::Guarded<int>& obj, int* seen_out)
{
    int seen = co_await obj.async([](const int& v) { return v; });        // read
    co_await obj.async([seen](int& v) { v += seen + 1; });                // write
    *seen_out = seen;
}

void test_thread_safe()
{
    ts::Guarded<int> obj{ ts::Named{}, 5 };
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

// 5. A `co_await` loop - the case continuations cannot express without recursion.
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
// resumed segment, so a body touching guarded data after a suspension does not fault the
// harness. The `Signal` gate makes the suspension + resume deterministic and, crucially,
// resumes outside the original `Access_scope` (below) - so `current_access` is null at resume
// and only the promise's re-installed snapshot lets `increment()`/`value()` pass.
Task<int> co_touch_after_await(tests::Counter& c, ts::Signal& gate)
{
    co_await gate;        // suspends until triggered
    c.increment();        // guarded - faults the harness without the re-installed grant
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
    }   // grant scope ends - the coroutine keeps its snapshot copy

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
    }   // guard released - pipe free again
    auto r = co_await ts::read_only(w);        // shared reader; `const Counter&`
    co_return r->value();
}

void test_write_guard()
{
    ts::Guarded<tests::Counter> w{ ts::Named{} };
    TS_CHECK(co_write_guard(w).sync() == 6);   // 1 + 5
}

// 9. Read guard: shared access, `const` view - the harness passes for a const method inside it.
Task<int> co_read_guard(ts::Guarded<tests::Counter>& w)
{
    auto g = co_await ts::read_only(w);
    co_return g->value();
}

void test_read_guard()
{
    ts::Guarded<tests::Counter> w{ ts::Named{} };
    w.async([](tests::Counter& c) { c.add(9); }).sync();
    TS_CHECK(co_read_guard(w).sync() == 9);
}

// 10. Guard + control flow: a loop inside one write-guard scope - the ergonomic win over an
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
    ts::Guarded<tests::Counter> w{ ts::Named{} };
    TS_CHECK(co_guard_loop(w, 10).sync() == 10);
}

// 11. Contention: many threads each drive a coroutine that repeatedly acquires the write guard
// on the same object. The pipe serializes the writers (deferred acquire -> suspend -> resume on
// the releasing thread), so the total is exact. The concurrency test.
Task<void> co_bump(ts::Guarded<tests::Counter>& w, int times)
{
    for (int i = 0; i < times; ++i)
    {
        auto g = co_await ts::read_write(w);   // may defer + resume cross-thread under contention
        g->increment();
    }   // released each iteration - no guard held across the next co_await
}

void test_guard_contention()
{
    ts::Guarded<tests::Counter> w{ ts::Named{} };
    constexpr int threads = 8, each = 200;
    {
        std::vector<std::jthread> drivers;
        for (int i = 0; i < threads; ++i)
            drivers.emplace_back([&w] { co_bump(w, each).sync(); });
    }   // join
    TS_CHECK(co_read_guard(w).sync() == threads * each);
}

// 12. The suspension detector: `co_await` other work while holding a guard faults. Subprocess
// death test (the fatal aborts) - the scenario lives in `run_death_scenario` (tests.cpp).
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

    // (b) a launched task forks work; the co_await gates on its completion.
    // `nested_sum` lives in the coroutine frame, so it outlives the launched task.
    std::atomic<int> nested_sum{ 0 };
    co_await ts::launch([&nested_sum]
    {
        for (int k = 0; k < 4; ++k)
            nested_sum.fetch_add(k, std::memory_order_relaxed);
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
        ts::Guarded<tests::Counter> world{ ts::Named{} };
        ts::Signal phase;
        ts::Cancellation_source src;
        Task<int> t = co_showcase(world, phase, src.token());
        phase.trigger();
        TS_CHECK(t.sync() == 113);
    }
    // Cancelled path: same pipeline, early-out after the phase gate.
    {
        ts::Guarded<tests::Counter> world{ ts::Named{} };
        ts::Signal phase;
        ts::Cancellation_source src;
        src.request_cancel();
        Task<int> t = co_showcase(world, phase, src.token());
        phase.trigger();
        TS_CHECK(t.sync() == -1);
    }
}

// 14. Pool-exhaustion proof: a recursive fork-join, expressed with `co_await`, that a
// blocking wait could not complete on a worker pool smaller than the join's depth/width --
// the exact deadlock class that retraction used to break by running the not-yet-started
// child inline on the blocked waiter. Coroutine-first eliminates that class structurally:
// a `co_await` on unfinished sub-work suspends (freeing the worker) instead of blocking, so
// a waiting frame never occupies a worker while its children need one.
//
// Each frame first hops onto a worker with `co_await ts::launch([]{})`, so every recursive
// call is an eager coroutine that immediately suspends on its own hop, becoming a queued
// unit a worker must pick up; when a frame later `co_await`s its two children it is itself
// running ON a worker. On one worker a blocking wait would be an instant classic deadlock
// (the sole worker runs a frame, the frame blocks on a child, the child can't run because
// the worker is occupied); with suspension the frame yields the worker, the worker runs the
// queued child, and its completion resumes the parent. (A blocking variant is deliberately
// not implemented: an in-task sync()/take() on unfinished work is fatal by design.)
constexpr int fork_join_leaves = 96;   // tree depth ~7, ~190 nodes - far exceeds 1-2 workers

Task<long long> co_sum_range(const int* values, int lo, int hi)
{
    co_await ts::launch([] {});   // hop onto a worker: this frame becomes a scheduled unit
    if (hi - lo <= 1)
        co_return values[lo];
    int mid = lo + (hi - lo) / 2;
    Task<long long> left = co_sum_range(values, lo, mid);
    Task<long long> right = co_sum_range(values, mid, hi);
    co_return co_await left + co_await right;   // suspends here, freeing the worker
}

void run_fork_join_on_pool(int workers)
{
    std::vector<int> values(fork_join_leaves);
    long long expected = 0;
    for (int i = 0; i < fork_join_leaves; ++i) { values[i] = i; expected += i; }

    // Run on a helper thread so a hang cannot wedge the suite: wait on the future with a
    // deadline and report a failure instead of blocking forever.
    std::promise<long long> prom;
    std::future<long long> fut = prom.get_future();
    std::thread runner([&]
    {
        ts::Scheduler_scope scope{ { .num_threads = static_cast<uint32_t>(workers) } };
        long long r = co_sum_range(values.data(), 0, fork_join_leaves).sync();
        prom.set_value(r);
    });

    if (fut.wait_for(std::chrono::seconds(20)) == std::future_status::ready)
    {
        runner.join();
        TS_CHECK(fut.get() == expected);
    }
    else
    {
        TS_CHECK(false && "fork-join hung: worker pool exhausted (suspension regression)");
        runner.detach();
    }
}

void test_fork_join_one_worker()  { run_fork_join_on_pool(1); }
void test_fork_join_two_workers() { run_fork_join_on_pool(2); }

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
    ts::Guarded<int> b{ ts::Named{}, 0 };
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
    ts::Guarded<int> d{ ts::Named{}, 0 };
    ts::Task<int> t = d.async([](const int& v) { return v; }, { .token = src.token() });
    TS_CHECK(await_cancelled_checked(std::move(t)).sync() == -1);
}

#if TS_SAFETY_CHECKS
void test_death_await_cancelled_value()
{
    TS_CHECK(ts::test::expect_death("await_cancelled_value"));
}
#endif

// TODO 6.10/6.11 - the two structural entry checks and their sanctioned forms.

#if TS_SAFETY_CHECKS
void test_death_sync_settled_in_task()
{
    TS_CHECK(ts::test::expect_death("sync_settled_in_task"));
}

void test_death_await_settled_under_guard()
{
    TS_CHECK(ts::test::expect_death("await_settled_under_guard"));
}
#endif

// Companion to `await_settled_under_guard`: split the scope. The guard is released before
// the await and re-acquired after - the object is free while the frame is suspended, which
// is what the rule enforces.
Task<int> await_under_guard_split(ts::Guarded<tests::Counter>& w, ts::Task<int> other)
{
    {
        auto g = co_await ts::read_write(w);
        g->increment();
    }                                   // guard released
    int value = co_await other;         // legal: nothing is held across the suspension
    {
        auto g = co_await ts::read_write(w);
        g->add(value);
        co_return g->value();
    }
}

void test_await_under_guard_split()
{
    ts::Guarded<tests::Counter> w{ ts::Named{ "w" } };
    ts::Guarded<int> value{ ts::Named{ "value" }, 4 };
    ts::Task<int> other = value.async([](const int& v) { return v; });
    TS_CHECK(await_under_guard_split(w, std::move(other)).sync() == 5);
}

// Companion to `sync_settled_in_task`, and the stated exemption to the guard rule: a
// reentrant same-object access runs inline under the held grant (waiting rule (b)), so its
// task is settled before the `co_await` is evaluated and cannot suspend by construction.
// That is the one shape `await_ready` lets through with a guard live.
Task<int> reentrant_access_under_guard(ts::Guarded<tests::Counter>& w)
{
    auto g = co_await ts::read_write(w);
    g->increment();
    // `w`'s writer_owner is this frame, so `access` takes the reentrant arm.
    int seen = co_await w.access([](const tests::Counter& k) { return k.value(); });
    co_return seen;
}

void test_reentrant_access_under_guard()
{
    ts::Guarded<tests::Counter> w{ ts::Named{ "w" } };
    TS_CHECK(reentrant_access_under_guard(w).sync() == 1);
}

// The other companion to that fatal, and the one that needs no polling: `as_optional()`
// waits like a plain `co_await` and yields an empty optional on cancellation.
Task<int> await_optional(ts::Task<int> maybe_cancelled)
{
    std::optional<int> value = co_await maybe_cancelled.as_optional();
    co_return value.value_or(-1);
}

void test_await_as_optional()
{
    ts::Guarded<int> d{ ts::Named{ "d" }, 5 };

    ts::Cancellation_source src;
    src.request_cancel();
    ts::Task<int> cancelled = d.async([](const int& v) { return v; }, { .token = src.token() });
    TS_CHECK(await_optional(std::move(cancelled)).sync() == -1);

    ts::Task<int> completed = d.async([](const int& v) { return v; });
    TS_CHECK(await_optional(std::move(completed)).sync() == 5);
}

// `try_take()` never blocks, so it is legal inside a task - the non-blocking spelling of
// "consume it if it happens to be ready".
void test_try_take()
{
    ts::Guarded<int> d{ ts::Named{ "d" }, 11 };

    // Spin rather than `gate.sync()`: a blocking wait inside a task is itself illegal.
    std::atomic<bool> release{ false };
    ts::Task<int> pending = ts::launch([&release]
    {
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
        return 3;
    });
    TS_CHECK(!pending.try_take().has_value());   // unsettled -> empty, no park
    release.store(true, std::memory_order_release);
    pending.sync();
    TS_CHECK(pending.try_take() == 3);

    ts::Cancellation_source src;
    src.request_cancel();
    ts::Task<int> cancelled = d.async([](const int& v) { return v; }, { .token = src.token() });
    while (!cancelled.is_done())
        std::this_thread::yield();
    TS_CHECK(!cancelled.try_take().has_value());   // cancelled -> empty, not fatal

    // Inside a task: never parks, so the in-task rule does not apply to it.
    ts::Task<int> settled = d.async([](const int& v) { return v; });
    settled.sync();
    std::atomic<int> seen{ -1 };
    ts::launch([&seen, settled]() mutable
    {
        seen.store(settled.try_take().value_or(-2), std::memory_order_relaxed);
    }).sync();
    TS_CHECK(seen.load(std::memory_order_relaxed) == 11);
}

// The reentrant access arm (coroutine-first §4.2, waiting rule (b)): `access` from a task that
// already holds the object's write grant runs inline under it instead of queueing behind
// itself (which used to be the sharp same-object deadlock).
void test_access_reentrant_under_own_grant()
{
    ts::Guarded<int> a{ ts::Named{}, 0 };
    ts::Static_task_graph g;
    std::atomic<int> seen{ -1 };
    g.add_node(ts::Named{}, [&a, &seen](int& v)
    {
        v = 5;
        ts::Task<int> r = a.access([](const int& x) { return x; });   // reentrant: inline, done
        TS_CHECK(r.is_done());
        // `sync()` inside a task is illegal even on a settled target (TODO 6.10 - the rule,
        // not the incident); `try_take()` is the non-blocking read.
        seen.store(r.try_take().value_or(-1));
    }, a);
    g.compile();
    g.execute().sync();
    TS_CHECK(seen.load() == 5);
}

// --- stage 2: coroutine nodes ---------------------------------------------

// The §4.4 shape: a coroutine node body - a data-parallel fan-out under the node's grant,
// a foreign read awaits under held grants, and the node completes (releasing grants,
// unlocking successors) only at frame completion.
void test_coroutine_graph_node()
{
    // The node holds `phys` and `result` and dynamically awaits `audio`, so all three carry a
    // `ts::Rank` and the awaited one is strictly highest - the waiting-rule (c) residual made
    // representable-but-ordered (TODO 6.14). Without ranks the await is refused: an unranked
    // held object cannot be climbed away from safely.
    ts::Guarded<std::vector<int>> phys{ ts::Named{}, ts::Rank{ 10 }, std::vector<int>{ 1, 2, 3 } };
    ts::Guarded<int> audio{ ts::Named{}, ts::Rank{ 30 }, 40 };
    ts::Guarded<int> result{ ts::Named{}, ts::Rank{ 20 }, 0 };
    std::atomic<int> total{ 0 };
    std::atomic<int> successor_runs{ 0 };

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [&audio, &total](const std::vector<int>& islands, int& out) -> ts::Task<void>
    {
        total.store(0);                                             // re-run-safe
        // Data-parallel fan-out over the node's owned read: the helpers inherit the node's
        // grant (Access_context snapshot), and the synchronous join gates the body - the one
        // sanctioned in-task wait (it waits on running work only).
        ts::parallel_for(islands.size(), [&](std::size_t i) { total.fetch_add(islands[i]); });
        TS_CHECK(total.load() == 6);

        int mix = co_await audio.access([](const int& a) { return a; });   // foreign read (c)
        out = total.load() + mix;
        co_return;
    }, phys, result);
    g.add_node(ts::Named{}, [&successor_runs](const int& out)
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

// Companion for the circular-wait fatal (docs/coroutine-first.md §2 hierarchy, step 1):
// the same cross-object communication with both objects declared - compile() derives the
// conflict edges and orders the nodes, so neither suspends and no cycle can form.
Task<void> declared_pair_body(tests::Counter& own, tests::Counter& other)
{
    own.increment();
    other.increment();
    co_return;
}

void test_cross_object_declared()
{
    ts::Guarded<tests::Counter> a{ ts::Named{} };
    ts::Guarded<tests::Counter> b{ ts::Named{} };
    ts::Static_task_graph graph;
    graph.add_node(ts::Named{}, [](tests::Counter& own, tests::Counter& other) { return declared_pair_body(own, other); }, a, b);
    graph.add_node(ts::Named{}, [](tests::Counter& own, tests::Counter& other) { return declared_pair_body(own, other); }, b, a);
    graph.compile();
    graph.execute().sync();
    TS_CHECK(a.access([](const tests::Counter& c) { return c.value(); }).sync() == 2);
    TS_CHECK(b.access([](const tests::Counter& c) { return c.value(); }).sync() == 2);
}

#if TS_SAFETY_CHECKS
// The suspended-ABBA deadlock: two coroutine nodes each holding a declared grant and
// awaiting the other's object. The circular-wait detector fatals on the closing edge - the
// only diagnosis this shape can get (no thread parks; the frames simply never resume).
void test_death_circular_wait()
{
    TS_CHECK(ts::test::expect_death("circular_wait"));
}
#endif

// --- Frame_gate (cross-frame realignment, coroutine-first.md §4.7) --------

ts::Task<void> gate_waiter(ts::Frame_gate& gate, std::atomic<int>& woke_at, std::atomic<int>& frame)
{
    co_await gate.next();
    woke_at.store(frame.load(std::memory_order_acquire), std::memory_order_release);
}

// A task parked on the gate resumes at the next boundary, not this one and not two later.
void test_frame_gate_releases_at_next_boundary()
{
    ts::Frame_gate gate;
    std::atomic<int> frame{ 0 }, woke_at{ -1 };

    ts::Task<void> waiter = gate_waiter(gate, woke_at, frame);
    TS_CHECK(!waiter.is_done());   // parked: no boundary has passed

    frame.store(1, std::memory_order_release);
    gate.open();
    waiter.sync();
    TS_CHECK(woke_at.load() == 1);

    // The gate re-arms: a second waiter parks on the fresh gate and needs its own boundary.
    std::atomic<int> woke2{ -1 };
    ts::Task<void> second = gate_waiter(gate, woke2, frame);
    TS_CHECK(!second.is_done());
    frame.store(2, std::memory_order_release);
    gate.open();
    second.sync();
    TS_CHECK(woke2.load() == 2);
}

// Many waiters across several frames: each is released by exactly one boundary, and a
// handle taken before a boundary is released BY it rather than missing it (the missed-wakeup
// window a hand-rolled trigger/reset pair has).
void test_frame_gate_many_waiters()
{
    constexpr int frames = 8, per_frame = 6;
    ts::Frame_gate gate;
    std::atomic<int> released{ 0 };

    std::vector<ts::Task<void>> waiters;
    for (int f = 0; f < frames; ++f)
    {
        for (int k = 0; k < per_frame; ++k)
        {
            waiters.push_back([](ts::Frame_gate& g, std::atomic<int>& count) -> ts::Task<void>
            {
                co_await g.next();
                count.fetch_add(1, std::memory_order_relaxed);
            }(gate, released));
        }
        gate.open();
        for (ts::Task<void>& w : waiters)
            w.sync();
        TS_CHECK(released.load() == (f + 1) * per_frame);
        waiters.clear();
    }
}

// `open()` on an idle gate is a no-op with no waiters to release, and the gate stays usable.
void test_frame_gate_idle_open()
{
    ts::Frame_gate gate;
    gate.open();
    gate.open();

    std::atomic<int> frame{ 5 }, woke_at{ -1 };
    ts::Task<void> waiter = gate_waiter(gate, woke_at, frame);
    gate.open();
    waiter.sync();
    TS_CHECK(woke_at.load() == 5);
}

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
    run("co await as_optional", test_await_as_optional);
    run("try_take non-blocking", test_try_take);
#if TS_SAFETY_CHECKS
    run_if(with_rule_in_task_sync, "TS_RULE_IN_TASK_SYNC off", "death: sync on a settled task in a task", test_death_sync_settled_in_task);
    run_if(with_rule_await_under_guard, "TS_RULE_AWAIT_UNDER_GUARD off", "death: await a settled task under a guard", test_death_await_settled_under_guard);
#endif
    run("co await under guard, split", test_await_under_guard_split);
    run("co reentrant access under guard", test_reentrant_access_under_guard);
#if TS_SAFETY_CHECKS
    run("death: await cancelled value", test_death_await_cancelled_value);
#endif
    run("co access reentrant under own grant", test_access_reentrant_under_own_grant);
    run("co graph node", test_coroutine_graph_node);
    run("co cross-object declared", test_cross_object_declared);
#if TS_SAFETY_CHECKS
    run_if(with_rule_circular_wait, "TS_RULE_CIRCULAR_WAIT off", "death: circular wait", test_death_circular_wait);
#endif
    run("co showcase", test_showcase);
    run("co fork-join 1 worker (pool-exhaustion proof)", test_fork_join_one_worker);
    run("co fork-join 2 workers (pool-exhaustion proof)", test_fork_join_two_workers);
    run("frame gate releases at next boundary", test_frame_gate_releases_at_next_boundary);
    run("frame gate many waiters", test_frame_gate_many_waiters);
    run("frame gate idle open", test_frame_gate_idle_open);
}
