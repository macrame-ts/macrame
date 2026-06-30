#include "integration_tests.h"
#include "thread_safe.h"
#include "static_task_graph.h"
#include "harness.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <tuple>
#include <vector>

using namespace std::chrono_literals;
using ts::test::run;
using tests::record_max;

namespace
{

int read_value(ts::Thread_safe<int>& d)
{
    return d.async([](const int& v) { return v; }).get();
}

// `then()` chained off the graph's `execute()` completion handle.
void test_then_off_graph_completion()
{
    ts::Thread_safe<int> a{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { v = 5; }, a);
    g.compile();

    std::atomic<bool> after{ false };
    g.execute().then([&after] { after.store(true); }).get();

    TS_CHECK(after.load());
    TS_CHECK(read_value(a) == 5);
}

// `when_all` over async results, feeding a value into a graph run.
void test_when_all_into_graph()
{
    ts::Thread_safe<int> a{ 2 }, b{ 3 };
    int sum = ts::when_all(
            a.async([](const int& v) { return v; }),
            b.async([](const int& v) { return v; }))
        .then([](std::tuple<int, int>& r) { return std::get<0>(r) + std::get<1>(r); })
        .get();

    ts::Thread_safe<int> c{ 0 };
    ts::Static_task_graph g;
    g.add_node([sum](int& v) { v = sum; }, c);
    g.compile();
    g.execute().get();

    TS_CHECK(read_value(c) == 5);
}

// graph run, then a dynamic async on the same object (sequential, no race).
void test_graph_then_dynamic()
{
    ts::Thread_safe<int> a{ 0 };
    ts::Static_task_graph g;
    g.add_node([](int& v) { v = 7; }, a);
    g.compile();
    g.execute().get();

    TS_CHECK(read_value(a) == 7);
}

// J: repeat a concurrency-sensitive workload to catch flakiness.
void test_repeat_stress()
{
    bool all = true;
    for (int iter = 0; iter < 20; ++iter)
    {
        std::atomic<int> active{ 0 }, peak{ 0 };
        ts::Thread_safe<int> data{ 0 };
        std::vector<ts::Task<int>> tasks;

        for (int i = 0; i < 8; ++i)
            tasks.push_back(data.async([&active, &peak](const int& v)
            {
                record_max(peak, active.fetch_add(1) + 1);
                std::this_thread::sleep_for(1ms);
                active.fetch_sub(1);
                return v;
            }));
        for (auto& t : tasks)
            t.get();

        all = all && (peak.load() > 1);
    }
    TS_CHECK(all);
}

} // namespace

void run_integration_tests()
{
    std::printf("\n[integration] tests\n");
    run("then off graph completion", test_then_off_graph_completion);
    run("when_all into graph", test_when_all_into_graph);
    run("graph then dynamic", test_graph_then_dynamic);
    run("repeat stress x20", test_repeat_stress);
}
