#include "deferred_tests.h"
#include "deferred.h"
#include "static_task_graph.h"
#include "harness.h"
#include "test_util.h"

#include <atomic>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

using ts::test::run;
using tests::Counter;

namespace
{

// --- compile-time contract --------------------------------------------------

static_assert(!std::is_copy_constructible_v<ts::Recorder<int>>, "recorder is a producer identity, move-only");
static_assert(std::is_move_constructible_v<ts::Recorder<int>>);
static_assert(!std::is_copy_constructible_v<ts::Deferred<int>>);
static_assert(std::is_same_v<
    decltype(std::declval<ts::Deferred<int>&>().commit_async()), ts::Task<void>>);

// --- basics -------------------------------------------------------------------

void test_stage_commit_applies_all()
{
    ts::Guarded<Counter> target;
    ts::Deferred<Counter> d{ target };

    auto rec = d.recorder();
    rec.stage([](Counter& c) { c.add(3); });
    rec.stage([](Counter& c) { c.add(4); });
    d.commit_async().sync();

    int v = target.async([](const Counter& c) { return c.value(); }).sync();
    TS_CHECK(v == 7);
}

void test_stage_takes_no_grant()
{
    // Staging happens on a thread with NO task context at all -- if it touched the
    // target, the harness would fatal. It must not.
    ts::Guarded<Counter> target;
    ts::Deferred<Counter> d{ target };
    auto rec = d.recorder();
    rec.stage([](Counter& c) { c.increment(); });
    d.commit_async().sync();
    TS_CHECK(target.async([](const Counter& c) { return c.value(); }).sync() == 1);
}

void test_empty_commit()
{
    ts::Guarded<int> target{ 9 };
    ts::Deferred<int> d{ target };
    d.commit_async().sync();
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 9);
}

void test_intra_recorder_fifo()
{
    ts::Guarded<std::vector<int>> target;
    ts::Deferred<std::vector<int>> d{ target };

    auto rec = d.recorder();
    for (int i = 0; i < 10; ++i)
        rec.stage([i](std::vector<int>& v) { v.push_back(i); });
    d.commit_async().sync();

    auto v = target.async([](const std::vector<int>& v) { return v; }).sync();
    TS_CHECK(v.size() == 10);
    for (int i = 0; i < 10; ++i)
        TS_CHECK(v[i] == i);
}

void test_recorder_creation_order_not_stage_order()
{
    // rec2's commands are staged chronologically FIRST, but rec1 was created first
    // -- the apply order is keyed by recorder creation, not by stage time.
    ts::Guarded<std::vector<int>> target;
    ts::Deferred<std::vector<int>> d{ target };

    auto rec1 = d.recorder();
    auto rec2 = d.recorder();
    rec2.stage([](std::vector<int>& v) { v.push_back(3); });
    rec2.stage([](std::vector<int>& v) { v.push_back(4); });
    rec1.stage([](std::vector<int>& v) { v.push_back(1); });
    rec1.stage([](std::vector<int>& v) { v.push_back(2); });
    d.commit_async().sync();

    auto v = target.async([](const std::vector<int>& v) { return v; }).sync();
    TS_CHECK((v == std::vector<int>{ 1, 2, 3, 4 }));
}

void test_concurrent_staging()
{
    constexpr int threads = 8;
    constexpr int per_thread = 500;

    ts::Guarded<Counter> target;
    ts::Deferred<Counter> d{ target };

    std::vector<std::jthread> staging;
    for (int t = 0; t < threads; ++t)
        staging.emplace_back([rec = d.recorder()]() mutable
        {
            for (int i = 0; i < per_thread; ++i)
                rec.stage([](Counter& c) { c.increment(); });
        });
    staging.clear();   // join

    d.commit_async().sync();
    int v = target.async([](const Counter& c) { return c.value(); }).sync();
    TS_CHECK(v == threads * per_thread);
}

// --- snapshot visibility on the pipe -----------------------------------------

void test_readers_see_none_then_all()
{
    ts::Guarded<int> target{ 0 };
    ts::Deferred<int> d{ target };

    auto rec = d.recorder();
    rec.stage([](int& v) { v = 42; });

    // Reader queued BEFORE the commit job: pipe FIFO runs it first -> pre-state.
    ts::Task<int> before = target.async([](const int& v) { return v; });
    ts::Task<void> commit = d.commit_async();
    ts::Task<int> after = target.async([](const int& v) { return v; });

    TS_CHECK(before.sync() == 0);
    commit.sync();
    TS_CHECK(after.sync() == 42);
}

// --- commit under an existing grant -------------------------------------------

void test_commit_under_async_write_grant()
{
    ts::Guarded<Counter> target;
    ts::Deferred<Counter> d{ target };

    auto rec = d.recorder();
    rec.stage([](Counter& c) { c.add(5); });

    // The `commit(T&)` form: no second pipe acquisition, applied under the write
    // hold the surrounding body already has.
    target.async([&d](Counter& c)
    {
        c.add(1);
        d.commit(c);
        c.add(1);
    }).sync();

    TS_CHECK(target.async([](const Counter& c) { return c.value(); }).sync() == 7);
}

void test_commit_in_graph_node()
{
    ts::Guarded<Counter> target;
    ts::Guarded<int> producer_state{ 0 };
    ts::Deferred<Counter> d{ target };

    ts::Static_task_graph g;
    auto producer = g.add_node([rec = d.recorder()](int& s) mutable
    {
        ++s;
        rec.stage([](Counter& c) { c.add(10); });
    }, producer_state);
    auto commit = g.add_node([&d](Counter& c) { d.commit(c); }, target);
    commit.after(producer);
    g.compile();

    g.execute().sync();
    g.execute().sync();

    TS_CHECK(target.async([](const Counter& c) { return c.value(); }).sync() == 20);
    TS_CHECK(producer_state.async([](const int& v) { return v; }).sync() == 2);
}

// --- cut semantics --------------------------------------------------------------

void test_straggler_rides_next_commit()
{
    ts::Guarded<int> target{ 0 };
    ts::Deferred<int> d{ target };

    auto rec = d.recorder();
    rec.stage([](int& v) { v += 1; });
    d.commit_async().sync();
    rec.stage([](int& v) { v += 2; });   // after the cut

    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 1);
    d.commit_async().sync();
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 3);
}

void test_cancelled_commit_retains_commands()
{
    ts::Cancellation_source src;
    src.request_cancel();

    ts::Guarded<int> target{ 0 };
    ts::Deferred<int> d{ target };

    auto rec = d.recorder();
    rec.stage([](int& v) { v = 7; });

    ts::Task<void> commit = d.commit_async({ .token = src.token() });
    commit.sync();   // cancelled void sync unblocks
    TS_CHECK(commit.is_cancelled());
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 0);

    // The skipped commit never cut -- the commands are still staged.
    d.commit_async().sync();
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 7);
}

void test_discard()
{
    ts::Guarded<int> target{ 0 };
    ts::Deferred<int> d{ target };

    auto rec = d.recorder();
    rec.stage([](int& v) { v = 99; });
    d.discard();
    d.commit_async().sync();
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 0);
}

void test_move_only_command()
{
    ts::Guarded<int> target{ 0 };
    ts::Deferred<int> d{ target };

    auto rec = d.recorder();
    rec.stage([p = std::make_unique<int>(31)](int& v) { v = *p; });
    d.commit_async().sync();
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 31);
}

void test_drop_staged_is_fatal()
{
    TS_CHECK(ts::test::expect_death("deferred_drop_staged"));
}

} // namespace

void run_deferred_tests()
{
    run("deferred: stage + commit applies all", test_stage_commit_applies_all);
    run("deferred: staging takes no grant", test_stage_takes_no_grant);
    run("deferred: empty commit is a no-op", test_empty_commit);
    run("deferred: intra-recorder FIFO", test_intra_recorder_fifo);
    run("deferred: apply keyed by recorder creation order", test_recorder_creation_order_not_stage_order);
    run("deferred: concurrent staging from many threads", test_concurrent_staging);
    run("deferred: readers see none before, all after", test_readers_see_none_then_all);
    run("deferred: commit under an async write grant", test_commit_under_async_write_grant);
    run("deferred: commit as a graph node (re-run)", test_commit_in_graph_node);
    run("deferred: straggler rides the next commit", test_straggler_rides_next_commit);
    run("deferred: cancelled commit retains commands", test_cancelled_commit_retains_commands);
    run("deferred: discard drops staged commands", test_discard);
    run("deferred: move-only command capture", test_move_only_command);
    run("deferred: destroy with staged commands is fatal", test_drop_staged_is_fatal);
}
