#include "tests.h"
#include "test_util.h"
#include "ts/access.h"
#include "ts/guarded.h"
#include "ts/static_task_graph.h"

#if defined(__cpp_impl_coroutine)
#include "ts/coroutine_support.h"
#endif

#include "scheduler_tests.h"
#include "access_tests.h"
#include "guarded_tests.h"
#include "task_tests.h"
#include "graph_tests.h"
#include "parallel_tests.h"
#include "coroutine_tests.h"
#include "integration_tests.h"
#include "deferred_tests.h"
#include "versioned_tests.h"

#include "ts/deferred.h"
#include "ts/versioned.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

void run_all_tests()
{
    run_scheduler_tests();
    run_access_tests();
    run_guarded_tests();
    run_task_tests();
    run_graph_tests();
    run_parallel_tests();
    run_coroutine_tests();
    run_integration_tests();
    run_deferred_tests();
    run_versioned_tests();
}

#if defined(__cpp_impl_coroutine)
// Death scenario body: acquire a `Guarded` write guard, then `co_await` other work while
// still holding it -- the pipe-held-across-suspension anti-pattern. Runs eagerly, so the fatal
// fires during the call below, before `sync()`. `never` is never triggered, so `co_await never`
// always reaches `await_suspend` (the detector) rather than escaping.
static ts::Task<int> coro_await_under_guard(ts::Guarded<tests::Counter>& w, ts::Signal& never)
{
    auto g = co_await ts::read_write(w);
    co_await never;              // suspend while the guard is held -> fatal
    co_return g->value();
}
#endif

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
        ts::Guarded<int> a{ 0 };
        ts::Static_task_graph g;
        g.add_node([&a](int&)
        {
            // Queues behind this node's own write hold -> never admitted; the sync
            // diagnostic fires the sharp same-object message, then the worker parks
            // forever. The child's main thread observes the report and aborts, so the
            // parent sees a death instead of a hang.
            a.async([](int& v) { v = 1; }).sync();
        }, a);
        g.compile();
        ts::Task<void> run = g.execute();
        for (int i = 0; i < 10000 && ts::ensure_failure_count() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (ts::ensure_failure_count() != 0)
            ts::fatal("sync_own_object_deadlock: sharp diagnostic observed");
        // Diagnostic never fired: fall through and exit 0 -- the parent's
        // `expect_death` then fails the test instead of hanging.
    }
#endif
    else if (std::strcmp(name, "guarded_outlived_by_graph") == 0)
    {
        ts::Static_task_graph g;
        {
            ts::Guarded<int> a{ 0 };
            g.add_node([](int&) {}, a);
            g.compile();
        }   // ~Guarded: a compiled graph still references it -> fatal
    }
    else if (std::strcmp(name, "graph_destroyed_mid_run") == 0)
    {
        ts::Guarded<int> a{ 0 };
        ts::Signal go;   // never triggered: the run stays in flight
        {
            ts::Static_task_graph g;
            g.add_node([&go](int&) { go.sync(); }, a);
            g.compile();
            ts::Task<void> run = g.execute();   // remaining_nodes set at execute entry
        }   // ~Static_task_graph with the run in flight -> fatal
    }
    else if (std::strcmp(name, "stale_inherited_grant") == 0)
    {
        ts::Guarded<Counter> c;
        ts::Signal go;
        ts::Task<void> stray;
        ts::Static_task_graph g;
        g.add_node([&stray, &go](Counter& k)
        {
            // Deliberately NOT `ts::nested`/`add_nested`: the task inherits the node's
            // grant but does not gate the node's completion. Gated on `go`, so it runs
            // only after the node has completed and released its write hold on `c`.
            stray = ts::task([&k] { k.increment(); }).after(go).launch();
        }, c);
        g.compile();
        g.execute().sync();   // node done; its write grant on `c` released (epoch moved)
        go.trigger();
        stray.sync();         // body writes `c` under the stale inherited grant -> fatal
    }
    else if (std::strcmp(name, "graph_cycle") == 0)
    {
        ts::Guarded<int> a{ 0 }, b{ 0 };
        ts::Static_task_graph g;
        ts::Graph_node na = g.add_node([](int&) {}, a);
        ts::Graph_node nb = g.add_node([](int&) {}, b);
        na.after(nb);
        nb.after(na);
        g.compile();     // cycle -> fatal
    }
    else if (std::strcmp(name, "execute_before_compile") == 0)
    {
        ts::Static_task_graph g;
        g.execute().sync();   // -> fatal
    }
    else if (std::strcmp(name, "graph_undeclared") == 0)
    {
        Counter outside;
        ts::Guarded<int> a{ 0 };
        ts::Static_task_graph g;
        g.add_node([&outside](int&) { outside.increment(); }, a);   // touches undeclared `outside`
        g.compile();
        g.execute().sync();   // node runs -> violation
    }
    else if (std::strcmp(name, "cancelled_value_get") == 0)
    {
        ts::Cancellation_source src;
        src.request_cancel();
        ts::Guarded<int> d{ 0 };
        ts::Task<int> t = d.async([](const int& v) { return v; }, { .token = src.token() });
        while (!t.is_done()) std::this_thread::yield();
        t.sync();   // cancelled value task has no result -> fatal
    }
    else if (std::strcmp(name, "add_nested_outside") == 0)
    {
        ts::Task<int> t = ts::launch([] { return 1; });
        t.sync();
        ts::add_nested(t);   // no currently-executing task -> fatal
    }
    else if (std::strcmp(name, "reset_unsettled") == 0)
    {
        auto t = ts::task([] { return 1; });   // built, not launched -> not settled
        t.reset();   // reset before the task has settled -> fatal
    }
    else if (std::strcmp(name, "deferred_drop_staged") == 0)
    {
        ts::Guarded<int> target{ 0 };
        ts::Deferred<int> d{ target };
        auto rec = d.recorder();
        rec.stage([](int& v) { ++v; });
        // `d` destroyed with a staged uncommitted command -> fatal (lost write)
    }
    else if (std::strcmp(name, "recorder_empty_stage") == 0)
    {
        ts::Guarded<int> target{ 0 };
        ts::Deferred<int> d{ target };
        ts::Recorder<int> rec = d.recorder();
        ts::Recorder<int> moved = std::move(rec);
        moved.stage([](int& v) { ++v; });
        rec.stage([](int& v) { ++v; });   // moved-from: empty -> fatal
    }
    else if (std::strcmp(name, "journal_slot_overflow") == 0)
    {
        ts::Guarded<int> target{ 0 };
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
        ts::Versioned<int> v;
        auto rec = v.recorder();
        rec.stage([](int& x) { ++x; });
        // destroyed with a staged unpublished command -> fatal (lost write)
    }
    else if (std::strcmp(name, "versioned_divergence") == 0)
    {
        ts::Versioned<int> v;
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
        // FATAL instead of racing its shadow apply. Pre-enforcement this child
        // drains and exits normally -- the parent's expect_death then fails.
        static std::atomic<bool> phase1_running{ false };
        static std::atomic<bool> release{ false };
        {
            ts::Versioned<int> v;
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
            g.add_node(ts::publish_body(v), v.state());
            g.compile();
            g.execute().sync();                     // flip catches the unresolved publish -> fatal

            release.store(true);                    // pre-enforcement path: drain and exit 0
            pending.sync();
            v.publish().sync();
        }
    }
    else if (std::strcmp(name, "versioned_wrong_front") == 0)
    {
        ts::Versioned<int> v;
        int other = 0;
        ts::Access_context ctx;
        ctx.add(&other, Access::read_write);
        ts::Access_scope scope(ctx);
        v.publish_into(other);   // not this Versioned's front -> fatal
    }
#if defined(__cpp_impl_coroutine)
    else if (std::strcmp(name, "coro_await_under_guard") == 0)
    {
        ts::Guarded<Counter> w;
        ts::Signal never;
        coro_await_under_guard(w, never).sync();   // fatals during the coroutine's eager run
    }
#endif
    // unknown scenario: return without dying -> parent's expect_death fails
}
