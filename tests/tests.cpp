#include "tests.h"
#include "test_util.h"
#include "ts/access.h"
#include "ts/guarded.h"
#include "ts/static_task_graph.h"

#include "ts/coroutine_support.h"

#include "scheduler_tests.h"
#include "access_tests.h"
#include "rules_tests.h"
#include "named_tests.h"
#include "guarded_tests.h"
#include "pipe_tests.h"
#include "task_tests.h"
#include "graph_tests.h"
#include "parallel_tests.h"
#include "coroutine_tests.h"
#include "integration_tests.h"
#include "deferred_tests.h"
#include "versioned_tests.h"
#include "event_bus_tests.h"

#include "ts/deferred.h"
#include "ts/versioned.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

void run_all_tests()
{
    run_scheduler_tests();
    run_access_tests();
    run_rules_tests();
    run_named_tests();
    run_guarded_tests();
    run_pipe_tests();
    run_task_tests();
    run_graph_tests();
    run_parallel_tests();
    run_coroutine_tests();
    run_integration_tests();
    run_deferred_tests();
    run_versioned_tests();
    run_event_bus_tests();
}

// Death scenario body: acquire a `Guarded` write guard, then `co_await` other work while
// still holding it - the pipe-held-across-suspension anti-pattern. Runs eagerly, so the fatal
// fires during the call below, before `sync()`. `never` is never triggered, so `co_await never`
// always reaches `await_suspend` (the detector) rather than escaping.
static ts::Task<int> coro_await_under_guard(ts::Guarded<tests::Counter>& w, ts::Signal& never)
{
    auto g = co_await ts::read_write(w);
    co_await never;              // suspend while the guard is held -> fatal
    co_return g->value();
}

// Death scenario body (`multi_guard_await_under_guard`): holds a multi-object guard over both
// objects and then awaits - the suspension detector faults, both objects held across the wait.
static ts::Task<void> multi_await_under_guard(ts::Guarded<tests::Counter>& a,
                                              ts::Guarded<tests::Counter>& b, ts::Signal& never)
{
    auto [x, y] = co_await ts::read_write(a, b);
    co_await never;              // suspend while both grants are held -> fatal
    x.add(1);
    y.add(1);
}

// Death scenario body (`stale_inherited_grant`): created inside a node body, so the frame
// inherits the node's grant; suspended on `go` (a non-nested stray), it resumes only after
// the node completed and released its hold - the write then runs under a stale inherited
// grant and the harness fatals.
static ts::Task<void> stale_stray(tests::Counter& k, ts::Signal go)
{
    co_await go;
    k.increment();
}

// Death scenario body (`circular_wait`): a coroutine graph-node body that touches its
// declared object, waits until both nodes hold their grants (the flag sync forces the
// overlap), then awaits the other node's object - the suspended-ABBA shape. Each deferred
// acquire records a wait edge; whichever inserts second closes the cycle and fatals.
static std::atomic<int> abba_holding{ 0 };
static ts::Task<void> abba_body(tests::Counter& own, ts::Guarded<tests::Counter>& other)
{
    // The rank rule (6.14) makes this shape unrepresentable - with valid ranks the two
    // awaits cannot both climb - so constructing it to test the circular-wait detector means
    // opting out of the rank rule for the body. That is the honest relationship between the
    // two: rank prevents, the detector is what remains for programs that opted out.
    ts::Relaxed_scope relax{ ts::Rule::access_rank };
    own.increment();
    abba_holding.fetch_add(1, std::memory_order_acq_rel);
    while (abba_holding.load(std::memory_order_acquire) < 2)
        std::this_thread::yield();   // both nodes hold before either awaits
    auto guard = co_await ts::read_write(other);   // defers behind the other node's grant -> cycle
    guard->increment();
}

// Death scenario body (`await_settled_under_guard`, TODO 6.11): holds a write guard and
// awaits a task that is already settled. Before the check was hoisted to `co_await` entry
// this ran clean - `await_ready` was true, so `await_suspend` (where the fatal lived) was
// never reached, and the illegal hold-then-await shipped undiagnosed on every run where
// timing was friendly. Companion: `test_await_under_guard_split` (coroutine_tests).
static ts::Task<void> await_settled_under_guard(ts::Guarded<tests::Counter>& w, ts::Task<int> settled)
{
    auto g = co_await ts::read_write(w);
    (void)co_await settled;   // settled -> would not suspend -> fatal anyway (the rule, not the incident)
    g->increment();
}

// Death scenario body (`signal_reset_awaited`): parks a coroutine on the signal so the
// re-arm-while-awaited misuse has a live awaiter.
static ts::Task<void> await_signal(ts::Signal s)
{
    co_await s;
}

void run_death_scenario(const char* name)
{
    using tests::Counter;
    using ts::Access;

    if (std::strcmp(name, "access_no_context") == 0)
    {
        Counter c;
        c.increment();   // no task context -> violation
    }
    else if (std::strcmp(name, "access_ro_write") == 0)
    {
        Counter c;
        ts::Access_context ctx;
        ctx.add(&c, Access::read_only);
        ts::Access_scope scope(ctx);
        c.increment();   // write under a read-only grant -> violation
    }
    else if (std::strcmp(name, "access_wrong_instance") == 0)
    {
        Counter c, other;
        ts::Access_context ctx;
        ctx.add(&other, Access::read_write);
        ts::Access_scope scope(ctx);
        c.increment();   // c not declared -> violation
    }
    else if (std::strcmp(name, "access_context_overflow") == 0)
    {
        int objs[9] = {};
        ts::Access_context ctx;
        for (int i = 0; i < 9; ++i)
            ctx.add(&objs[i], Access::read_write);   // 9th add overflows `max_entries` -> fatal
    }
#if TS_SAFETY_CHECKS
    else if (std::strcmp(name, "sync_own_object_deadlock") == 0)
    {
        ts::Guarded<int> a{ ts::Named{}, 0 };
        ts::Static_task_graph g;
        g.add_node(ts::Named{}, [&a](int&)
        {
            // Queues behind this node's own write hold -> never admitted. The sync
            // diagnostic fires the sharp same-object message and aborts on the worker.
            // (The bounded wait below is the pre-6.10 fallback, when the diagnostic was a
            // report rather than a fatal; it keeps a regression from hanging the parent.)
            a.async([](int& v) { v = 1; }).sync();
        }, a);
        g.compile();
        ts::Task<void> run = g.execute();
        for (int i = 0; i < 10000 && ts::ensure_failure_count() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (ts::ensure_failure_count() != 0)
            ts::fatal("sync_own_object_deadlock: sharp diagnostic observed");
        // Diagnostic never fired: fall through and exit 0 - the parent's
        // `expect_death` then fails the test instead of hanging.
    }
#endif
    else if (std::strcmp(name, "guarded_outlived_by_graph") == 0)
    {
        ts::Static_task_graph g;
        {
            ts::Guarded<int> a{ ts::Named{}, 0 };
            g.add_node(ts::Named{}, [](int&) {}, a);
            g.compile();
        }   // ~Guarded: a compiled graph still references it -> fatal
    }
    else if (std::strcmp(name, "graph_destroyed_mid_run") == 0)
    {
        ts::Guarded<int> a{ ts::Named{}, 0 };
        ts::Signal go;   // never triggered: the run stays in flight
        {
            ts::Static_task_graph g;
            g.add_node(ts::Named{}, [&go](int&) { go.sync(); }, a);
            g.compile();
            ts::Task<void> run = g.execute();   // remaining_nodes set at execute entry
        }   // ~Static_task_graph with the run in flight -> fatal
    }
    else if (std::strcmp(name, "stale_inherited_grant") == 0)
    {
        ts::Guarded<Counter> c{ ts::Named{} };
        ts::Signal go;
        ts::Task<void> stray;
        ts::Static_task_graph g;
        g.add_node(ts::Named{}, [&stray, &go](Counter& k)
        {
            // A detached coroutine: it inherits the node's grant (its promise snapshots the
            // ambient context at creation, here inside the body) but does not gate the node's
            // completion. Suspended on `go`, it resumes only after the node has completed and
            // released its write hold on `c`.
            stray = stale_stray(k, go);
        }, c);
        g.compile();
        g.execute().sync();   // node done; its write grant on `c` released (epoch moved)
        go.trigger();
        stray.sync();         // body writes `c` under the stale inherited grant -> fatal
    }
    else if (std::strcmp(name, "detached_launch_undeclared") == 0)
    {
        // A detached `ts::launch` inherits no grant (docs/coroutine-first.md §2): a child that
        // touches the launcher's guarded data runs under an empty context, so the harness
        // faults on the first access - deterministically, in every checked run. (Contrast the
        // stale-grant scenario above, where an inherited grant lets an early touch pass and
        // only a late touch faults; a detached child has no such timing window.)
        ts::Guarded<Counter> c{ ts::Named{} };
        ts::Task<void> child;
        c.access([&child](Counter& k)
        {
            child = ts::launch([&k] { k.add(1); });   // detached: touches the launcher's data
        }).sync();
        child.sync();   // blue-thread wait keeps us alive until the child runs -> undeclared-access fatal
    }
    else if (std::strcmp(name, "graph_lend_mode_conflict") == 0)
    {
        // The outer node declares read on `x`; the inner graph writes it. A read grant
        // cannot be lent to a writer, and upgrading would re-acquire behind the caller's
        // own hold - fatal at the nested `execute()`, not a deadlock later.
        ts::Guarded<int> x{ ts::Named{}, 0 };
        ts::Static_task_graph inner;
        inner.add_node(ts::Named{}, [](int& v) { v += 1; }, x);
        inner.compile();

        ts::Static_task_graph outer;
        outer.add_node(ts::Named{}, [&inner](const int& v) -> ts::Task<void>
        {
            (void)v;
            co_await inner.execute();   // -> fatal
        }, x);
        outer.compile();
        outer.execute().sync();
    }
    else if (std::strcmp(name, "graph_lend_unquiet_scope") == 0)
    {
        // Lending while an earlier un-awaited nested run of the caller is still in flight: that
        // run is executing under the same grant, so it could race the lent-to graph on the lent
        // object. `hold` never releases, so the first run stays live (its node spin-waits rather
        // than blocking on a `Signal` - an in-task `sync()` is itself fatal, which would kill it
        // for the wrong reason); the second run's `execute()` fatals on the non-quiet scope.
        ts::Guarded<int> x{ ts::Named{}, 0 };
        static std::atomic<bool> hold{ true };   // never cleared: the first run stays live
        ts::Static_task_graph inner_live;
        inner_live.add_node(ts::Named{}, [](int& v) { (void)v; while (hold.load(std::memory_order_relaxed)) std::this_thread::yield(); }, x);
        inner_live.compile();

        ts::Static_task_graph inner_lend;
        inner_lend.add_node(ts::Named{}, [](int& v) { v += 1; }, x);
        inner_lend.compile();

        ts::Static_task_graph outer;
        outer.add_node(ts::Named{}, [&inner_live, &inner_lend](int& v) -> ts::Task<void>
        {
            (void)v;
            (void)inner_live.execute();    // deliberately un-awaited: joins the frame's scope, stays live
            co_await inner_lend.execute(); // scope not quiet -> fatal
        }, x);
        outer.compile();
        outer.execute().sync();
    }
    else if (std::strcmp(name, "graph_execute_in_flight") == 0)
    {
        // One run at a time: a node calling `execute()` on its own graph re-enters while the
        // first run is still in flight (the shared `Run_state` would be overwritten).
        ts::Guarded<int> x{ ts::Named{}, 0 };
        ts::Static_task_graph g;
        g.add_node(ts::Named{}, [&g](int& v)
        {
            (void)v;
            (void)g.execute();   // -> fatal
        }, x);
        g.compile();
        g.execute().sync();
    }
    else if (std::strcmp(name, "graph_cycle") == 0)
    {
        ts::Guarded<int> a{ ts::Named{}, 0 }, b{ ts::Named{}, 0 };
        ts::Static_task_graph g;
        ts::Graph_node na = g.add_node(ts::Named{}, [](int&) {}, a);
        ts::Graph_node nb = g.add_node(ts::Named{}, [](int&) {}, b);
        na.after(nb);
        nb.after(na);
        g.compile();     // cycle -> fatal
    }
    else if (std::strcmp(name, "execute_before_compile") == 0)
    {
        ts::Static_task_graph g;
        g.execute().sync();   // -> fatal
    }
    else if (std::strcmp(name, "graph_compile_twice") == 0)
    {
        ts::Guarded<int> a{ ts::Named{}, 0 };
        ts::Static_task_graph g;
        g.add_node(ts::Named{}, [](int& v) { ++v; }, a);
        g.compile();
        g.compile();   // -> fatal: build-once
    }
    else if (std::strcmp(name, "graph_add_node_after_compile") == 0)
    {
        ts::Guarded<int> a{ ts::Named{}, 0 };
        ts::Static_task_graph g;
        g.add_node(ts::Named{}, [](int& v) { ++v; }, a);
        g.compile();
        g.add_node(ts::Named{}, [](int& v) { ++v; }, a);   // -> fatal: build-once
    }
    else if (std::strcmp(name, "graph_duplicate_object") == 0)
    {
        ts::Guarded<int> a{ ts::Named{}, 0 };
        ts::Static_task_graph g;
        g.add_node(ts::Named{}, [](int& v, const int& r) { v = r; }, a, a);   // -> fatal: duplicate
    }
    else if (std::strcmp(name, "async_duplicate_object") == 0)
    {
        ts::Guarded<int> a{ ts::Named{}, 0 };
        ts::async([](int& v, const int& r) { v = r; }, a, a).sync();   // -> fatal: duplicate
    }
    else if (std::strcmp(name, "multi_guard_duplicate_object") == 0)
    {
        ts::Guarded<int> a{ ts::Named{}, 0 };
        [](ts::Guarded<int>& x) -> ts::Task<void>
        {
            auto g = co_await ts::read_write(x, x);   // -> fatal: duplicate
        }(a).sync();
    }
    else if (std::strcmp(name, "graph_undeclared") == 0)
    {
        Counter outside;
        ts::Guarded<int> a{ ts::Named{}, 0 };
        ts::Static_task_graph g;
        g.add_node(ts::Named{}, [&outside](int&) { outside.increment(); }, a);   // touches undeclared `outside`
        g.compile();
        g.execute().sync();   // node runs -> violation
    }
    else if (std::strcmp(name, "cancelled_value_get") == 0)
    {
        ts::Cancellation_source src;
        src.request_cancel();
        ts::Guarded<int> d{ ts::Named{}, 0 };
        ts::Task<int> t = d.async([](const int& v) { return v; }, { .token = src.token() });
        while (!t.is_done()) std::this_thread::yield();
        t.sync();   // cancelled value task has no result -> fatal
    }
    else if (std::strcmp(name, "reset_unsettled") == 0)
    {
        ts::Signal s;   // never triggered -> not settled
        s.reset();      // re-arm before the signal has settled -> fatal
    }
#if TS_SAFETY_CHECKS
    else if (std::strcmp(name, "sync_in_task") == 0)
    {
        // A node body parking on a pipe job for an object the task does not hold: fatal
        // (coroutine-first §4.1). The blocker keeps `b`'s pipe busy so the sync genuinely
        // parks (no timing dependence: the fatal fires before the wait).
        ts::Guarded<tests::Counter> a{ ts::Named{} };
        ts::Guarded<int> b{ ts::Named{}, 0 };
        std::atomic<bool> release{ false };
        ts::Static_task_graph g;
        g.add_node(ts::Named{}, [&b, &release](tests::Counter&)
        {
            ts::Task<void> blocker = b.async([&release](int&)
            {
                while (!release.load(std::memory_order_relaxed))
                    std::this_thread::yield();
            });
            b.async([](int& v) { v = 7; return v; }).sync();   // parks inside a task -> fatal
        }, a);
        g.compile();
        g.execute().sync();
    }
#endif
    else if (std::strcmp(name, "await_cancelled_value") == 0)
    {
        // Awaiting a cancelled Task<R> with non-void R: no result to produce, no
        // exceptions - a precondition, mirroring sync(). Companion:
        // `test_await_cancelled_checked` (coroutine_tests).
        ts::Cancellation_source src;
        src.request_cancel();
        ts::Guarded<int> d{ ts::Named{}, 0 };
        ts::Task<int> t = d.async([](const int& v) { return v; }, { .token = src.token() });
        while (!t.is_done())
            std::this_thread::yield();
        [](ts::Task<int> cancelled) -> ts::Task<void>
        {
            co_await cancelled;   // -> fatal
        }(std::move(t)).sync();
    }
    else if (std::strcmp(name, "deferred_drop_staged") == 0)
    {
        ts::Guarded<int> target{ ts::Named{}, 0 };
        ts::Deferred<int> d{ target };
        auto rec = d.recorder();
        rec.stage([](int& v) { ++v; });
        // `d` destroyed with a staged uncommitted command -> fatal (lost write)
    }
    else if (std::strcmp(name, "deferred_dtor_inflight_commit") == 0)
    {
        // If the in-flight fatal regresses, the destructor parks on the blocked
        // pipe instead of aborting; exit 0 after a bound so the parent's
        // `expect_death` fails rather than hanging.
        std::thread([]
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            std::_Exit(0);
        }).detach();

        ts::Guarded<int> target{ ts::Named{}, 0 };
        std::atomic<bool> release{ false };
        // Holds the pipe's write slot so the commit job cannot run or settle.
        ts::Task<void> blocker = target.async([&release](int&)
        {
            while (!release.load(std::memory_order_relaxed))
                std::this_thread::yield();
        });
        {
            ts::Deferred<int> d{ target };
            auto rec = d.recorder();
            rec.stage([](int& v) { ++v; });
            d.commit();   // queued behind the blocker: still in flight
            // `d` destroyed with the commit unsettled -> fatal (before the
            // staged-leftover check can fire)
        }
    }
#if TS_SAFETY_CHECKS
    else if (std::strcmp(name, "deferred_commit_nested_grant") == 0)
    {
        ts::Guarded<int> target{ ts::Named{}, 0 };
        ts::Deferred<int> d{ target };
        auto rec = d.recorder();
        rec.stage([](int& v) { ++v; });
        ts::Static_task_graph g;
        g.add_node(ts::Named{}, [&d](int&)
        {
            // A detached coroutine inherits the node's write grant (its promise snapshots the
            // ambient context at creation) but is not the grant holder (`writer_owner` is the
            // node, `current_task` is the frame); its commit() would enqueue behind the node's
            // own hold -> the misuse diagnostic fatals at the call (during the eager body).
            [&d]() -> ts::Task<void> { d.commit(); co_return; }();
        }, target);
        g.compile();
        g.execute().sync();
    }
#endif
    else if (std::strcmp(name, "recorder_empty_stage") == 0)
    {
        ts::Guarded<int> target{ ts::Named{}, 0 };
        ts::Deferred<int> d{ target };
        ts::Recorder<int> rec = d.recorder();
        ts::Recorder<int> moved = std::move(rec);
        moved.stage([](int& v) { ++v; });
        rec.stage([](int& v) { ++v; });   // moved-from: empty -> fatal
    }
    else if (std::strcmp(name, "journal_slot_overflow") == 0)
    {
        ts::Guarded<int> target{ ts::Named{}, 0 };
        ts::Deferred<int> d{ target };
        std::vector<ts::Recorder<int>> alive;   // all kept alive: nothing recycles
        for (std::size_t i = 0; i <= ts::detail::Journal<int>::max_slots; ++i)
            alive.push_back(d.recorder());      // crossing max_slots -> fatal
    }
    else if (std::strcmp(name, "parallel_recorder_empty_stage") == 0)
    {
        ts::Parallel_recorder<int> rec;   // never bound
        rec.stage([](int& v) { ++v; });   // -> fatal
    }
    else if (std::strcmp(name, "versioned_drop_staged") == 0)
    {
        ts::Versioned<int> v{ ts::Named{} };
        auto rec = v.recorder();
        rec.stage([](int& x) { ++x; });
        // destroyed with a staged unpublished command -> fatal (lost write)
    }
    else if (std::strcmp(name, "versioned_dtor_inflight_publish") == 0)
    {
        // If the in-flight fatal regresses, the destructor parks on the blocked front
        // instead of aborting; exit 0 after a bound so the parent's `expect_death`
        // fails rather than hanging.
        std::thread([]
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            std::_Exit(0);
        }).detach();

        std::atomic<bool> release{ false };
        {
            ts::Versioned<int> v{ ts::Named{} };
            // A spinning reader holds the front, so the publish's swap (a write) cannot
            // run and the returned task stays unsettled.
            ts::Task<void> blocker = v.read([&release](const int&)
            {
                while (!release.load(std::memory_order_relaxed))
                    std::this_thread::yield();
            });
            auto rec = v.recorder();
            rec.stage([](int& x) { ++x; });
            v.publish();   // swap queued behind the reader: returned task in flight
            // v destroyed here with the publish unsettled -> fatal (before the
            // staged-leftover check can fire)
        }
    }
    else if (std::strcmp(name, "versioned_divergence") == 0)
    {
        ts::Versioned<int> v{ ts::Named{} };
        v.set_divergence_check([](const int& x) { return static_cast<std::size_t>(x); });
        auto rec = v.recorder();
        // Nondeterministic command: the two replay applications see different
        // `n`, so the replicas diverge -> fatal at the post-resync hash compare.
        rec.stage([](int& x) { static int n = 41; x = ++n; });
        v.publish().sync();
    }
    else if (std::strcmp(name, "versioned_mixed_publish") == 0)
    {
        // A dynamic publish whose phase 1 is parked (the staged command spins on
        // a flag) is invisible to the pipe; a graph flip that catches it must
        // fatal instead of racing its shadow apply. Pre-enforcement this child
        // drains and exits normally - the parent's expect_death then fails.
        static std::atomic<bool> phase1_running{ false };
        static std::atomic<bool> release{ false };
        {
            ts::Versioned<int> v{ ts::Named{} };
            auto rec = v.recorder();
            rec.stage([](int& x)
            {
                phase1_running.store(true);
                while (!release.load())
                    std::this_thread::yield();
                x += 1;
            });
            ts::Task<void> pending = v.publish();   // fire-and-forget: phase 1 parks on a worker
            while (!phase1_running.load())
                std::this_thread::yield();          // now provably unresolved

            auto rec2 = v.recorder();
            rec2.stage([](int& x) { x += 10; });
            ts::Static_task_graph g;
            g.add_node(ts::Named{}, ts::publish_fn(v), v.state());
            g.compile();
            g.execute().sync();                     // flip catches the unresolved publish -> fatal

            release.store(true);                    // pre-enforcement path: drain and exit 0
            pending.sync();
            v.publish().sync();
        }
    }
    else if (std::strcmp(name, "versioned_wrong_front") == 0)
    {
        ts::Versioned<int> v{ ts::Named{} };
        int other = 0;
        ts::Access_context ctx;
        ctx.add(&other, Access::read_write);
        ts::Access_scope scope(ctx);
        v.publish_into(other);   // not this Versioned's front -> fatal
    }
    else if (std::strcmp(name, "coro_await_under_guard") == 0)
    {
        ts::Guarded<Counter> w{ ts::Named{} };
        ts::Signal never;
        coro_await_under_guard(w, never).sync();   // fatals during the coroutine's eager run
    }
    else if (std::strcmp(name, "multi_guard_await_under_guard") == 0)
    {
        ts::Guarded<Counter> a{ ts::Named{} };
        ts::Guarded<Counter> b{ ts::Named{} };
        ts::Signal never;
        multi_await_under_guard(a, b, never).sync();   // fatals during the coroutine's eager run
    }
    else if (std::strcmp(name, "circular_wait") == 0)
    {
        // Two independent coroutine graph nodes, each holding its declared object and
        // awaiting the other's (see `abba_body`). The circular-wait detector fatals on the
        // closing edge. Companion: `test_cross_object_declared` (coroutine_tests) --
        // declare both objects and let compile() order the nodes.
        ts::Guarded<Counter> a{ ts::Named{ "objA" } };
        ts::Guarded<Counter> b{ ts::Named{ "objB" } };
        ts::Static_task_graph graph;
        // Named, so the fatal identifies both participants by node rather than by block
        // pointer - the coroutine body's frame inherits its node's identity.
        graph.add_node("nodeA", [&b](Counter& own) { return abba_body(own, b); }, a);
        graph.add_node("nodeB", [&a](Counter& own) { return abba_body(own, a); }, b);
        graph.compile();
        graph.execute().sync();
    }
    else if (std::strcmp(name, "deadlock_net_suspended") == 0)
    {
#if TS_RULE_ON(TS_RULE_DEADLOCK_NET)
        // The shape tier 2 structurally cannot see: a node suspends on a plain task await
        // (a Signal nobody triggers) while holding its declared grant. No pipe suspension,
        // so no wait edge - only the suspension registry has it. The report should
        // name the node, what it holds, and what it awaits.
        ts::Guarded<int> held{ ts::Named{ "held_object" }, 0 };
        ts::Signal never{ "never_triggered" };
        ts::Static_task_graph g;
        g.add_node("stuck", [&never](int& v) -> ts::Task<void>
        {
            v = 1;
            co_await never;   // suspends holding `held_object`; nothing will ever trigger it
        }, held);
        g.compile();
        ts::set_deadlock_net_window(std::chrono::milliseconds(200));
        g.execute().sync();   // -> the net fires from this boundary wait
#endif
    }
    else if (std::strcmp(name, "access_rank_descends") == 0)
    {
#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
        // A node holds the higher-ranked object and dynamically awaits a lower one: the
        // await descends, which is what makes a wait cycle representable. Rejected on the
        // first offending await, deterministically - no second half of a cycle needed.
        ts::Guarded<Counter> high{ ts::Named{ "high" }, ts::Rank{ 30 } };
        ts::Guarded<Counter> low{ ts::Named{ "low" }, ts::Rank{ 10 } };
        ts::Static_task_graph g;
        g.add_node("descend", [&low](Counter& own) -> ts::Task<void>
        {
            own.increment();
            co_await low.access([](const Counter& k) { return k.value(); });   // -> fatal
        }, high);
        g.compile();
        g.execute().sync();
#endif
    }
    else if (std::strcmp(name, "access_rank_unranked") == 0)
    {
#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
        // The strict default: the held object has no declared rank, so no dynamic await is
        // permitted while it is held - there is no order to climb.
        ts::Guarded<Counter> held{ ts::Named{ "held" } };
        ts::Guarded<Counter> target{ ts::Named{ "target" }, ts::Rank{ 50 } };
        ts::Static_task_graph g;
        g.add_node("unranked", [&target](Counter& own) -> ts::Task<void>
        {
            own.increment();
            co_await target.access([](const Counter& k) { return k.value(); });   // -> fatal
        }, held);
        g.compile();
        g.execute().sync();
#endif
    }
    else if (std::strcmp(name, "deadlock_net") == 0)
    {
#if TS_RULE_ON(TS_RULE_DEADLOCK_NET)
        // A blue thread waits on a Signal nothing will ever trigger, with the pool idle and
        // nothing registered as externally completable. Progress is impossible; the net
        // notices from the waiter itself. Without it this child would hang and the parent's
        // `expect_death` would time out rather than fail.
        ts::launch([] {}).sync();   // make sure the scheduler exists and has gone quiet
        ts::set_deadlock_net_window(std::chrono::milliseconds(200));
        ts::Signal never;
        never.sync();   // -> fatal
#endif
    }
    else if (std::strcmp(name, "sync_settled_in_task") == 0)
    {
        // TODO 6.10: an in-task `sync()` whose target is already settled. The old check was
        // gated on "the wait would genuinely park", so this shape passed in development and
        // parked a worker only on the frame a prerequisite ran long. Now it is the rule that
        // fires, deterministically on the first execution.
        ts::Guarded<int> value{ ts::Named{ "value" }, 4 };
        ts::launch([&value]
        {
            ts::Task<int> settled = value.async([](const int& v) { return v; });
            while (!settled.is_done())
                std::this_thread::yield();
            (void)settled.sync();   // settled, and still fatal -> the rule, not the incident
        }).sync();
    }
    else if (std::strcmp(name, "await_settled_under_guard") == 0)
    {
        ts::Guarded<Counter> w{ ts::Named{ "w" } };
        ts::Guarded<int> value{ ts::Named{ "value" }, 1 };
        ts::Task<int> settled = value.async([](const int& v) { return v; });
        settled.sync();   // blue thread: legal
        await_settled_under_guard(w, std::move(settled)).sync();
    }
    else if (std::strcmp(name, "signal_reset_awaited") == 0)
    {
        // Re-arming a Signal while a coroutine is suspended on it: the signal has not
        // settled, so `reset()` hits the not-settled fatal - the awaited-side variant of
        // `reset_unsettled`. Companion: `test_signal_reset` (settle, then reset).
        ts::Signal s;
        ts::Task<void> t = await_signal(s);   // suspends (never triggered)
        s.reset();   // -> fatal
    }
#if TS_SAFETY_CHECKS
    else if (std::strcmp(name, "access_op_start_unbound") == 0)
    {
        struct Read_fn { int operator()(const int& v) const { return v; } };
        ts::Access_op<int, Read_fn> op;
        op.start();   // no target, no body -> fatal
    }
    else if (std::strcmp(name, "access_op_sync_never_started") == 0)
    {
        struct Read_fn { int operator()(const int& v) const { return v; } };
        ts::Guarded<int> d{ ts::Named{}, 1 };
        ts::Access_op<int, Read_fn> op(ts::dormant, d, Read_fn{});
        (void)op.sync();   // dormant: the wait would hang forever -> fatal
    }
    else if (std::strcmp(name, "access_op_double_consume") == 0)
    {
        ts::Guarded<int> d{ ts::Named{}, 2 };
        auto op = d.access([](const int& v) { return v; });   // free pipe: settled inline
        (void)op.sync();
        (void)op.sync();   // second consume of the same cycle -> fatal
    }
#endif
#if TS_RULE_ON(TS_RULE_IN_TASK_SYNC)
    else if (std::strcmp(name, "access_op_dtor_in_task") == 0)
    {
        // Destroying an in-flight op INSIDE a task: the dtor's wait is a blocking sync, so
        // the blue boundary applies - fatal via `blocking_sync_diagnose`, not ensure+wait
        // (a worker parking here risks exhaustion deadlock). The gate is never released:
        // the fatal aborts first; were the check absent the dtor would wait forever, so the
        // scenario is compiled only with the rule (the run_if registration names it).
        ts::Guarded<int> x{ ts::Named{ "x" }, 0 };
        ts::Guarded<int> y{ ts::Named{ "y" }, 0 };
        std::atomic<bool> gate{ false };
        ts::Task<void> blocker = y.async([&gate](int&) { while (!gate.load()) std::this_thread::yield(); });
        x.async([&y](int&)
        {
            auto op = y.access([](const int& v) { return v; });   // queued behind the blocker
        }).sync();   // -> fatal on the worker at op's destruction
    }
#endif
    // unknown scenario: return without dying -> parent's expect_death fails
}
