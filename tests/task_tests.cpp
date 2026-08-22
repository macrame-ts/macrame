#include "task_tests.h"
#include "ts/guarded.h"
#include "ts/coroutine_support.h"
#include "harness.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using ts::test::run;
using namespace ts::test;
using tests::wait_until;

namespace
{

ts::Task<int> read_async(ts::Guarded<int>& d)
{
    return d.async([](const int& v) { return v; });
}

// --- E: handles: sync / is_done / awaiting ---------------------------------

void test_get_void()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };
    ts::Task<void> t = d.async([](int& v) { v = 1; });
    t.sync();
    TS_CHECK(read_async(d).sync() == 1);
}

void test_is_done()
{
    std::atomic<bool> go{ false };
    ts::Guarded<int> d{ ts::Named{}, 5 };
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

// The coroutine-first continuation: await the producer, transform, await the next stage --
// the linear spelling of the old `then` chain.
ts::Task<int> transform_chain(ts::Guarded<int>& d)
{
    int v = co_await d.async([](const int& x) { return x; });
    int doubled = co_await ts::launch([v] { return v * 2; });
    co_return doubled + 1;
}

void test_await_transform_chain()
{
    ts::Guarded<int> d{ ts::Named{}, 21 };
    TS_CHECK(transform_chain(d).sync() == 43);
}

// --- F: awaited joins (the coroutine-first `when_all`) ---------------------

// The tasks run eagerly and concurrently; sequential awaits complete when the last one
// does, in any settle order.
ts::Task<int> join_two(ts::Task<int> a, ts::Task<int> b)
{
    co_return co_await a + co_await b;
}

void test_await_join_two()
{
    ts::Guarded<int> a{ ts::Named{}, 10 }, b{ ts::Named{}, 32 };
    TS_CHECK(join_two(read_async(a), read_async(b)).sync() == 42);
}

void test_await_join_out_of_order()
{
    ts::Guarded<int> a{ ts::Named{}, 1 }, b{ ts::Named{}, 2 };
    ts::Task<int> ta = a.async([](const int& v) { std::this_thread::sleep_for(20ms); return v; });
    ts::Task<int> tb = b.async([](const int& v) { return v; });   // completes first
    TS_CHECK(join_two(std::move(ta), std::move(tb)).sync() == 3);
}

// A void producer joins as pure ordering; the value producer carries the result.
ts::Task<int> join_mixed(ts::Task<void> v, ts::Task<int> r)
{
    co_await v;
    co_return co_await r;
}

void test_await_join_void_prereq()
{
    ts::Guarded<int> a{ ts::Named{}, 10 }, b{ ts::Named{}, 32 };
    std::atomic<int> side{ 0 };
    ts::Task<void> v = a.async([&side](int& x) { side.store(x); });
    ts::Task<int> r = b.async([](const int& x) { return x; });

    TS_CHECK(join_mixed(std::move(v), std::move(r)).sync() == 32);
    TS_CHECK(side.load() == 10);    // the void producer ran
}

// A cancelled producer in a join: the void await resumes (query `is_cancelled`); a value
// await must check first (awaiting a cancelled value task is fatal - see coroutine_tests).
ts::Task<int> join_with_cancelled(ts::Task<int> ok, ts::Task<int> maybe)
{
    int base = co_await ok;
    while (!maybe.is_done())
        co_await ts::launch([] {});   // cheap re-check hop; keeps the example non-blocking
    if (maybe.is_cancelled())
        co_return base;
    co_return base + co_await maybe;
}

void test_await_join_cancelled_prereq()
{
    ts::Cancellation_source src;
    src.request_cancel();

    ts::Guarded<int> a{ ts::Named{}, 1 }, b{ ts::Named{}, 2 };
    ts::Task<int> ta = a.async([](const int& x) { return x; });                              // completes
    ts::Task<int> tb = b.async([](const int& x) { return x; }, { .token = src.token() });    // cancelled

    TS_CHECK(join_with_cancelled(std::move(ta), std::move(tb)).sync() == 1);
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

// Awaiting a signal: the coroutine suspends until the trigger (the continuation form).
ts::Task<void> await_signal_then_store(ts::Signal s, std::atomic<int>& sink, int value)
{
    co_await s;
    sink.store(value);
}

void test_signal_await()
{
    ts::Signal s;
    std::atomic<int> sink{ 0 };
    ts::Task<void> t = await_signal_then_store(s, sink, 42);
    TS_CHECK(sink.load() == 0);  // the coroutine waits for the trigger
    s.trigger();
    t.sync();
    TS_CHECK(sink.load() == 42);
}

void test_signal_await_after_trigger()
{
    ts::Signal s;
    s.trigger();
    std::atomic<int> sink{ 0 };
    await_signal_then_store(s, sink, 7).sync();   // settled signal: await_ready, no suspension
    TS_CHECK(sink.load() == 7);
}

void test_signal_idempotent_trigger()
{
    ts::Signal s;
    std::atomic<int> fired{ 0 };
    // Join on the observer task, not just the signal: `s.sync()` waits for the signal to
    // settle, not for work resumed by it.
    ts::Task<void> fired_done = await_signal_then_store(s, fired, 1);

    // Many concurrent triggers must complete the signal exactly once.
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < 8; ++i)
            threads.emplace_back([&] { s.trigger(); });
    }
    s.sync();
    fired_done.sync();   // the observer has now run
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

    ts::Guarded<int> d{ ts::Named{}, 0 };
    std::atomic<bool> ran{ false };
    ts::Task<int> t = d.async([&ran](int& v) { ran.store(true); return v; }, { .token = src.token() });

    wait_until([&] { return t.is_done(); });
    TS_CHECK(t.is_cancelled());
    TS_CHECK(!ran.load());              // body skipped
}

void test_cancel_void_get_unblocks()
{
    ts::Cancellation_source src;
    src.request_cancel();

    ts::Guarded<int> d{ ts::Named{}, 0 };
    ts::Task<void> t = d.async([](int& v) { v = 1; }, { .token = src.token() });
    t.sync();                            // void: unblocks, no fatal
    TS_CHECK(t.is_cancelled());
}

void test_not_cancelled_normally()
{
    ts::Guarded<int> d{ ts::Named{}, 7 };
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

// TODO 7.3 regression guard: a task's captured resources must be destroyed before its `sync()`
// returns. `Executable::run` now destroys the body (its `union { Body }` storage) right after
// the invoke, before the task settles - so the closure and everything it captured are gone
// before a `sync()`/`co_await` is woken, instead of lingering until the block's refcount hits
// zero (on a worker, after the caller moved on). Before the fix that lag let a captured
// `Recorder` outlive the `sync()` meant to bound it and race the next frame's `World`.
//
// Deterministic, no race detector: `t` is held across the check, so the block's refcount is
// >= 1 and it cannot be freed here. Without the fix nothing else destroys the body, so
// `destroyed == 0` (guaranteed, not racy - no path runs `~Executable` while a handle is
// live). With the fix the body is destroyed in `run()` before settle, so `destroyed == 1`.
void test_captures_destroyed_before_sync()
{
    std::atomic<int> destroyed{ 0 };
    struct Probe
    {
        std::atomic<int>* d;
        explicit Probe(std::atomic<int>* p) : d(p) {}
        Probe(Probe&& o) noexcept : d(o.d) { o.d = nullptr; }   // a moved-from Probe doesn't count
        ~Probe() { if (d) d->fetch_add(1, std::memory_order_relaxed); }
    };

    ts::Task<void> t = ts::launch([p = Probe{ &destroyed }] { (void)p; });
    t.sync();
    // `t` is still alive -> the block holds a ref -> it is not freed -> without the fix the
    // captured `Probe` cannot have been destroyed yet, so this observes 0.
    TS_CHECK(destroyed.load(std::memory_order_relaxed) == 1);
}

// `sync()` returns `const R&` and does not consume: the same task can be sync'd repeatedly,
// and readers (another sync, an awaiting coroutine) all see the same result.
void test_sync_const_ref_multi()
{
    ts::Task<int> t = ts::launch([] { return 42; });
    const int& a = t.sync();
    const int& b = t.sync();          // second sync - still valid (non-consuming)
    TS_CHECK(a == 42 && b == 42);
    TS_CHECK(&a == &b);               // same storage: sync() aliased the block's result

    ts::Task<int> plus = [](ts::Task<int> src) -> ts::Task<int>
    {
        co_return co_await src + 1;
    }(t);
    TS_CHECK(plus.sync() == 43);      // an awaiting reader sees the un-consumed result
    TS_CHECK(t.sync() == 42);         // and it is still there afterwards
}

// `take()` moves the result out - works for a move-only `R`, and steals the payload.
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

// The result is moved out at most once. `try_take()` answers empty on a second consume
// (its shape is "answer, do not assert"); the second `take()` is the fatal, covered by the
// `task_double_take` death scenario.
void test_try_take_second_consume_is_empty()
{
    ts::Task<std::unique_ptr<int>> t = ts::launch([] { return std::make_unique<int>(7); });
    t.sync();                             // settle it, so `try_take` is not merely early
    std::optional first = t.try_take();
    TS_CHECK(first && *first && **first == 7);
    TS_CHECK(!t.try_take().has_value());
}

// `co_await t.as_optional()` consumes like `take()`, so it claims the same flag: a following
// `try_take()` reads empty rather than handing back the moved-from result.
ts::Task<bool> consume_via_as_optional(ts::Task<std::unique_ptr<int>> t)
{
    std::optional first = co_await t.as_optional();
    co_return first && *first && **first == 7 && !t.try_take().has_value();
}

void test_as_optional_claims_the_consume()
{
    ts::Task<std::unique_ptr<int>> t = ts::launch([] { return std::make_unique<int>(7); });
    TS_CHECK(consume_via_as_optional(t).sync());
}

void test_death_double_take() { TS_CHECK(ts::test::expect_death("task_double_take")); }

void test_launch_priority()
{
    // ts::Priority is accepted on every launch route (ordering is covered deterministically by
    // the scheduler + graph tests; here just confirm the API threads through and runs).
    TS_CHECK(ts::launch([] { return 1; }, { .priority = ts::Priority::high }).sync() == 1);

    ts::Guarded<int> d{ ts::Named{}, 40 };
    TS_CHECK(d.async([](const int& v) { return v + 2; }, { .priority = ts::Priority::high }).sync() == 42);
}

// A task body that declares a trailing `Cancellation_token` receives the task's token and
// can poll it to early-out. Cancellation arrives while the body runs (the pre-run skip
// does not apply - the task already started), so this exercises the body parameter, not
// the dispatch-time skip. A cooperative early-out returns normally, so the task completes.
void test_task_body_token_earlyout()
{
    ts::Cancellation_source src;
    std::atomic<bool> started{ false };
    std::atomic<int> stage{ 0 };

    ts::Task<void> t = ts::launch([&](ts::Cancellation_token tok)
    {
        started.store(true);
        while (!tok.is_cancel_requested())
            std::this_thread::yield();   // running - poll the token
        stage.store(1);                  // observed cancellation mid-body -> early out
    }, { .token = src.token() });

    wait_until([&] { return started.load(); });   // body started before we cancel
    src.request_cancel();
    t.sync();
    TS_CHECK(stage.load() == 1);          // the body received and reacted to the token
    TS_CHECK(!t.is_cancelled());          // cooperative return -> completed, not cancelled
}

// An async read accessor may take a trailing token and early-out mid-run.
void test_async_body_token_earlyout()
{
    ts::Cancellation_source src;
    std::atomic<bool> started{ false };
    std::atomic<int> stage{ 0 };

    ts::Guarded<int> d{ ts::Named{}, 5 };
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
    ts::Guarded<int> d{ ts::Named{}, 0 };
    d.async([](int& v, ts::Cancellation_token) { v = 9; }).sync();   // mutates -> write path
    TS_CHECK(d.async([](const int& v) { return v; }).sync() == 9);
}

void test_launch_cancelled()
{
    ts::Cancellation_source src;
    src.request_cancel();
    std::atomic<bool> ran{ false };
    ts::Task<int> t = ts::launch([&ran] { ran.store(true); return 1; }, { .token = src.token() });
    wait_until([&] { return t.is_done(); });
    TS_CHECK(t.is_cancelled());
    TS_CHECK(!ran.load());
}

// --- J: reusable phase gate -------------------------------------------------

// A Signal re-armed via reset() acts as a reusable phase gate (the one sanctioned reuse:
// bodyless, no result, no dispatch machinery to re-arm).
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
    TS_CHECK(ts::test::expect_death("reset_unsettled"));   // reset() before the signal settled
}

void test_death_signal_reset_awaited()
{
    // The awaited-side variant: a coroutine is suspended on the signal when reset() hits
    // the same not-settled guard. Companion: `test_signal_reset` (settle, then reset).
    TS_CHECK(ts::test::expect_death("signal_reset_awaited"));
}

// The body boundary, one case per path that invokes a user body (`detail::invoke_user_body`,
// and the coroutine promise's `unhandled_exception` for the frame arm). An exception leaving a
// body would unwind through the library's own frames - a worker's dispatch loop, a pipe
// release - past the grants, lock counts and refcounts they hold, so it is reported and the
// process aborts instead. The library is exception-agnostic: these fatals are what let a
// consumer compile their own translation units either way.
void test_death_body_throws_launch()   { TS_CHECK(ts::test::expect_death("body_throws_launch")); }
void test_death_body_throws_access()   { TS_CHECK(ts::test::expect_death("body_throws_access")); }
void test_death_body_throws_node()     { TS_CHECK(ts::test::expect_death("body_throws_node")); }
void test_death_body_throws_parallel() { TS_CHECK(ts::test::expect_death("body_throws_parallel_for")); }
void test_death_body_throws_coroutine(){ TS_CHECK(ts::test::expect_death("body_throws_coroutine")); }

// The same boundary on the result path: a body returns a type whose move constructor throws,
// and that move is what lands the value in the task's storage. Each scenario runs its body
// inline and catches: an exception the seam let past exits the child 0, so a hole fails these
// tests instead of dying by the runtime's own abort and reading as a pass.
void test_death_result_move_throws_launch()
{
    TS_CHECK(ts::test::expect_death("body_result_move_throws_launch"));
}

void test_death_result_move_throws_access()
{
    TS_CHECK(ts::test::expect_death("body_result_move_throws_access"));
}

} // namespace

void run_task_tests()
{
    std::printf("\n[task] tests\n");
    run("get void", test_get_void);
    run("is_done", test_is_done);
    run("await transform chain", test_await_transform_chain);
    run("await join two", test_await_join_two);
    run("await join out of order", test_await_join_out_of_order);
    run("await join void prereq", test_await_join_void_prereq);
    run("await join cancelled prereq", test_await_join_cancelled_prereq);
    run("signal trigger then wait", test_signal_trigger_then_wait);
    run("signal wait then trigger", test_signal_wait_then_trigger);
    run("signal await", test_signal_await);
    run("signal await after trigger", test_signal_await_after_trigger);
    run("signal idempotent trigger", test_signal_idempotent_trigger);
    run("signal copies share state", test_signal_copies_share_state);
    run("signal multiple waiters", test_signal_multiple_waiters);
    run("cancel before run", test_cancel_before_run);
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
    // TODO 7.3 regression guard: the body (and its captured resources) is destroyed in the
    // typed `Executable::run`, before the task settles, so it cannot outlive a `sync()`.
    run("captures destroyed before sync", test_captures_destroyed_before_sync);
    run("sync const-ref multi-consumer", test_sync_const_ref_multi);
    run("take moves move-only", test_take_moves_move_only);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "try_take second consume is empty",
           test_try_take_second_consume_is_empty);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "as_optional claims the consume",
           test_as_optional_claims_the_consume);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "death: double take", test_death_double_take);
    run("launch priority", test_launch_priority);
    run("task body token earlyout", test_task_body_token_earlyout);
    run("async body token earlyout", test_async_body_token_earlyout);
    run("async write token", test_async_write_token);
    run("launch cancelled", test_launch_cancelled);
    run("signal reset", test_signal_reset);
    run("death: reset unsettled", test_death_reset_unsettled);
    run("death: signal reset while awaited", test_death_signal_reset_awaited);
    run_if(with_exceptions, "built without exceptions", "death: body throws (launch)",
           test_death_body_throws_launch);
    run_if(with_exceptions, "built without exceptions", "death: body throws (access)",
           test_death_body_throws_access);
    run_if(with_exceptions, "built without exceptions", "death: body throws (graph node)",
           test_death_body_throws_node);
    run_if(with_exceptions, "built without exceptions", "death: body throws (parallel_for)",
           test_death_body_throws_parallel);
    run_if(with_exceptions, "built without exceptions", "death: body throws (coroutine)",
           test_death_body_throws_coroutine);
    run_if(with_exceptions, "built without exceptions", "death: body result move throws (launch)",
           test_death_result_move_throws_launch);
    run_if(with_exceptions, "built without exceptions", "death: body result move throws (access)",
           test_death_result_move_throws_access);
}
