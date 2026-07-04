#include "tests.h"
#include "test_util.h"
#include "access.h"
#include "thread_safe.h"
#include "static_task_graph.h"

#include "scheduler_tests.h"
#include "access_tests.h"
#include "thread_safe_tests.h"
#include "task_tests.h"
#include "graph_tests.h"
#include "integration_tests.h"

#include <cstring>
#include <thread>

void run_all_tests()
{
    run_scheduler_tests();
    run_access_tests();
    run_thread_safe_tests();
    run_task_tests();
    run_graph_tests();
    run_integration_tests();
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
    else if (std::strcmp(name, "graph_cycle") == 0)
    {
        ts::Thread_safe<int> a{ 0 }, b{ 0 };
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
        ts::Thread_safe<int> a{ 0 };
        ts::Static_task_graph g;
        g.add_node([&outside](int&) { outside.increment(); }, a);   // touches undeclared `outside`
        g.compile();
        g.execute().sync();   // node runs -> violation
    }
    else if (std::strcmp(name, "cancelled_value_get") == 0)
    {
        ts::Cancellation_source src;
        src.request_cancel();
        ts::Thread_safe<int> d{ 0 };
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
    // unknown scenario: return without dying -> parent's expect_death fails
}
