#include "tests.h"
#include "test_util.h"
#include "access.h"
#include "guarded.h"
#include "static_task_graph.h"

#if defined(__cpp_impl_coroutine)
#include "coroutine_support.h"
#endif

#include "scheduler_tests.h"
#include "access_tests.h"
#include "guarded_tests.h"
#include "task_tests.h"
#include "graph_tests.h"
#include "parallel_tests.h"
#include "coroutine_tests.h"
#include "integration_tests.h"

#include <cstring>
#include <thread>

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
}

#if defined(__cpp_impl_coroutine)
// Death scenario body: acquire a `Guarded` write guard, then `co_await` other work while
// still holding it -- the pipe-held-across-suspension anti-pattern. Runs eagerly, so the fatal
// fires during the call below, before `sync()`. `never` is never triggered, so `co_await never`
// always reaches `await_suspend` (the detector) rather than escaping.
static ts::Task<int> coro_await_under_guard(ts::Guarded<tests::Counter>& w, ts::Signal& never)
{
    auto g = co_await ts::write(w);
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
