#include "deferred_tests.h"
#include "ts/deferred.h"
#include "ts/versioned.h"
#include "ts/parallel_for.h"
#include "ts/static_task_graph.h"
#include "ts/coroutine_support.h"
#include "harness.h"
#include "test_util.h"

#include <atomic>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

using ts::test::run;
using namespace ts::test;
using tests::Counter;

namespace
{

// --- compile-time contract --------------------------------------------------

static_assert(!std::is_copy_constructible_v<ts::Recorder<int>>, "recorder is a producer identity, move-only");
static_assert(std::is_move_constructible_v<ts::Recorder<int>>);
static_assert(!std::is_copy_constructible_v<ts::Deferred<int>>);
static_assert(std::is_same_v<
    decltype(std::declval<ts::Deferred<int>&>().commit()), ts::Task<void>>);

// --- basics -------------------------------------------------------------------

void test_stage_commit_applies_all()
{
    ts::Guarded<Counter> target{ ts::Named{} };
    ts::Deferred<Counter> d{ target };

    auto rec = d.recorder();
    rec.stage([](Counter& c) { c.add(3); });
    rec.stage([](Counter& c) { c.add(4); });
    d.commit().sync();

    int v = target.async([](const Counter& c) { return c.value(); }).sync();
    TS_CHECK(v == 7);
}

void test_stage_takes_no_grant()
{
    // Staging happens on a thread with NO task context at all - if it touched the
    // target, the harness would fatal. It must not.
    ts::Guarded<Counter> target{ ts::Named{} };
    ts::Deferred<Counter> d{ target };
    auto rec = d.recorder();
    rec.stage([](Counter& c) { c.increment(); });
    d.commit().sync();
    TS_CHECK(target.async([](const Counter& c) { return c.value(); }).sync() == 1);
}

void test_empty_commit()
{
    ts::Guarded<int> target{ ts::Named{}, 9 };
    ts::Deferred<int> d{ target };
    d.commit().sync();
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 9);
}

void test_intra_recorder_fifo()
{
    ts::Guarded<std::vector<int>> target{ ts::Named{} };
    ts::Deferred<std::vector<int>> d{ target };

    auto rec = d.recorder();
    for (int i = 0; i < 10; ++i)
        rec.stage([i](std::vector<int>& v) { v.push_back(i); });
    d.commit().sync();

    auto v = target.async([](const std::vector<int>& v) { return v; }).sync();
    TS_CHECK(v.size() == 10);
    for (int i = 0; i < 10; ++i)
        TS_CHECK(v[i] == i);
}

void test_recorder_creation_order_not_stage_order()
{
    // rec2's commands are staged chronologically first, but rec1 was created first
    // - the apply order is keyed by recorder creation, not by stage time.
    ts::Guarded<std::vector<int>> target{ ts::Named{} };
    ts::Deferred<std::vector<int>> d{ target };

    auto rec1 = d.recorder();
    auto rec2 = d.recorder();
    rec2.stage([](std::vector<int>& v) { v.push_back(3); });
    rec2.stage([](std::vector<int>& v) { v.push_back(4); });
    rec1.stage([](std::vector<int>& v) { v.push_back(1); });
    rec1.stage([](std::vector<int>& v) { v.push_back(2); });
    d.commit().sync();

    auto v = target.async([](const std::vector<int>& v) { return v; }).sync();
    TS_CHECK((v == std::vector<int>{ 1, 2, 3, 4 }));
}

void test_concurrent_staging()
{
    constexpr int threads = 8;
    constexpr int per_thread = 500;

    ts::Guarded<Counter> target{ ts::Named{} };
    ts::Deferred<Counter> d{ target };

    std::vector<std::jthread> staging;
    for (int t = 0; t < threads; ++t)
    {
        staging.emplace_back([rec = d.recorder()]() mutable
        {
            for (int i = 0; i < per_thread; ++i)
                rec.stage([](Counter& c) { c.increment(); });
        });
    }
    staging.clear();   // join

    d.commit().sync();
    int v = target.async([](const Counter& c) { return c.value(); }).sync();
    TS_CHECK(v == threads * per_thread);
}

// --- snapshot visibility on the pipe -----------------------------------------

void test_readers_see_none_then_all()
{
    ts::Guarded<int> target{ ts::Named{}, 0 };
    ts::Deferred<int> d{ target };

    auto rec = d.recorder();
    rec.stage([](int& v) { v = 42; });

    // Reader queued before the commit job: pipe FIFO runs it first -> pre-state.
    ts::Task<int> before = target.async([](const int& v) { return v; });
    ts::Task<void> commit = d.commit();
    ts::Task<int> after = target.async([](const int& v) { return v; });

    TS_CHECK(before.sync() == 0);
    commit.sync();
    TS_CHECK(after.sync() == 42);
}

// --- commit under an existing grant -------------------------------------------

void test_commit_under_async_write_grant()
{
    ts::Guarded<Counter> target{ ts::Named{} };
    ts::Deferred<Counter> d{ target };

    auto rec = d.recorder();
    rec.stage([](Counter& c) { c.add(5); });

    // The `commit()` form: no second pipe acquisition, applied under the write
    // hold the surrounding body already has.
    target.async([&d](Counter& c)
    {
        c.add(1);
        (void)d.commit();
        c.add(1);
    }).sync();

    TS_CHECK(target.async([](const Counter& c) { return c.value(); }).sync() == 7);
}

void test_commit_under_coroutine_write_grant()
{
    ts::Guarded<Counter> target{ ts::Named{} };
    ts::Deferred<Counter> d{ target };

    auto rec = d.recorder();
    rec.stage([](Counter& c) { c.add(5); });

    // A `commit()` from inside a `co_await ts::read_write` guard: the guard published this
    // coroutine (not its launcher) as the target's `writer_owner`, so `commit()` recognises
    // itself as the grant holder, applies the batch inline under the held write, and returns an
    // already-settled task - the same fast path `test_commit_under_async_write_grant` takes from
    // a callback body, reached here through the coroutine guard.
    [](ts::Guarded<Counter>& tgt, ts::Deferred<Counter>& def) -> ts::Task<void>
    {
        auto g = co_await ts::read_write(tgt);
        g->add(1);
        ts::Task<void> committed = def.commit();
        TS_CHECK(committed.is_done());   // the inline held-grant arm returns a settled task
        g->add(1);
        TS_CHECK(g->value() == 7);       // batch (+5) applied synchronously, visible under the guard
    }(target, d).sync();

    TS_CHECK(target.async([](const Counter& c) { return c.value(); }).sync() == 7);
}

void test_commit_in_graph_node()
{
    ts::Guarded<Counter> target{ ts::Named{} };
    ts::Guarded<int> producer_state{ ts::Named{}, 0 };
    ts::Deferred<Counter> d{ target };

    ts::Static_task_graph g;
    auto producer = g.add_node(ts::Named{}, [rec = d.recorder()](int& s) mutable
    {
        ++s;
        rec.stage([](Counter& c) { c.add(10); });
    }, producer_state);
    auto commit = g.add_node(ts::Named{}, [&d](Counter&) { (void)d.commit(); }, target);
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
    ts::Guarded<int> target{ ts::Named{}, 0 };
    ts::Deferred<int> d{ target };

    auto rec = d.recorder();
    rec.stage([](int& v) { v += 1; });
    d.commit().sync();
    rec.stage([](int& v) { v += 2; });   // after the cut

    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 1);
    d.commit().sync();
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 3);
}

void test_cancelled_commit_retains_commands()
{
    ts::Cancellation_source src;
    src.request_cancel();

    ts::Guarded<int> target{ ts::Named{}, 0 };
    ts::Deferred<int> d{ target };

    auto rec = d.recorder();
    rec.stage([](int& v) { v = 7; });

    ts::Task<void> commit = d.commit({ .token = src.token() });
    commit.sync();   // cancelled void sync unblocks
    TS_CHECK(commit.is_cancelled());
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 0);

    // The skipped commit never cut - the commands are still staged.
    d.commit().sync();
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 7);
}

void test_discard()
{
    ts::Guarded<int> target{ ts::Named{}, 0 };
    ts::Deferred<int> d{ target };

    auto rec = d.recorder();
    rec.stage([](int& v) { v = 99; });
    d.discard();
    d.commit().sync();
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 0);
}

void test_move_only_command()
{
    ts::Guarded<int> target{ ts::Named{}, 0 };
    ts::Deferred<int> d{ target };

    auto rec = d.recorder();
    rec.stage([p = std::make_unique<int>(31)](int& v) { v = *p; });
    d.commit().sync();
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 31);
}

void test_parallel_recorder_from_parallel_for()
{
    // One logical producer parallelized over the pool: every stage lands
    // (per-worker slots, guided chunking, caller participates via the overflow
    // lane), applied exactly once.
    constexpr int n = 20000;
    ts::Guarded<Counter> target{ ts::Named{} };
    ts::Deferred<Counter> d{ target };

    auto rec = d.parallel_recorder();
    ts::parallel_for(n, [&rec](int) { rec.stage([](Counter& c) { c.increment(); }); });
    d.commit().sync();

    TS_CHECK(target.async([](const Counter& c) { return c.value(); }).sync() == n);
}

void test_parallel_recorder_overflow_lane()
{
    // Non-worker threads (this test thread + an external jthread) route to the
    // shared overflow slot - contended but correct.
    ts::Guarded<Counter> target{ ts::Named{} };
    ts::Deferred<Counter> d{ target };

    auto rec = d.parallel_recorder();
    rec.stage([](Counter& c) { c.add(1); });   // main thread: not a worker
    {
        std::jthread ext([&rec] { rec.stage([](Counter& c) { c.add(2); }); });
    }
    d.commit().sync();
    TS_CHECK(target.async([](const Counter& c) { return c.value(); }).sync() == 3);
}

void test_parallel_recorder_on_versioned()
{
    // Parallel staging into a Versioned: placement is nondeterministic but the
    // batch order is fixed at the cut, so replay resync applies the identical
    // sequence twice - the divergence check must stay quiet.
    constexpr int n = 5000;
    ts::Versioned<int> v{ ts::Named{} };
    v.set_divergence_check([](const int& x) { return static_cast<std::size_t>(x); });

    auto rec = v.parallel_recorder();
    ts::parallel_for(n, [&rec](int) { rec.stage([](int& x) { ++x; }); });
    v.publish().sync();
    v.publish().sync();   // resync fence (runs the divergence check to completion)

    TS_CHECK(v.read([](const int& x) { return x; }).sync() == n);
}

void test_released_slot_commands_survive()
{
    // A recorder destroyed with staged-but-uncommitted commands: the slot is
    // recycled, the commands drain on the next cut - nothing is lost.
    ts::Guarded<int> target{ ts::Named{}, 0 };
    ts::Deferred<int> d{ target };
    {
        auto rec = d.recorder();
        rec.stage([](int& v) { v += 5; });
    }   // released before any commit
    d.commit().sync();
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 5);
}

void test_slot_reuse_inherits_position()
{
    // r1 released -> r3 reuses r1's slot -> r3's commands apply at slot 0,
    // before r2's (slot 1), even though r3 was minted last. The documented
    // reuse-inherits-position semantics, pinned.
    ts::Guarded<std::vector<int>> target{ ts::Named{} };
    ts::Deferred<std::vector<int>> d{ target };

    auto r1 = d.recorder();
    auto r2 = d.recorder();
    r1 = ts::Recorder<std::vector<int>>{};   // release r1's slot (assign-over releases)
    auto r3 = d.recorder();                  // reuses slot 0

    r2.stage([](std::vector<int>& v) { v.push_back(2); });
    r3.stage([](std::vector<int>& v) { v.push_back(1); });
    d.commit().sync();

    auto v = target.async([](const std::vector<int>& v) { return v; }).sync();
    TS_CHECK((v == std::vector<int>{ 1, 2 }));
}

void test_recorder_churn_does_not_grow_journal()
{
    // Mint/stage/destroy far past max_slots (4096): the free-list recycles one
    // slot the whole way - no threshold fatal, every command applied.
    constexpr int churn = static_cast<int>(ts::detail::Journal<int>::max_slots) + 1000;
    ts::Guarded<int> target{ ts::Named{}, 0 };
    ts::Deferred<int> d{ target };

    for (int i = 0; i < churn; ++i)
    {
        auto rec = d.recorder();
        rec.stage([](int& v) { ++v; });
    }
    d.commit().sync();
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == churn);
}

void test_parallel_recorder_churn()
{
    // Same for the per-worker handle: each mint takes worker_count+1 slots, each
    // destroy returns them.
    ts::Guarded<int> target{ ts::Named{}, 0 };
    ts::Deferred<int> d{ target };

    for (int i = 0; i < 2000; ++i)
    {
        auto rec = d.parallel_recorder();
        rec.stage([](int& v) { ++v; });
    }
    d.commit().sync();
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 2000);
}

void test_slot_overflow_is_fatal()
{
    TS_CHECK(ts::test::expect_death("journal_slot_overflow"));
}

void test_late_bound_recorder()
{
    // The empty state's purpose: a member declared before the Deferred exists,
    // bound in init. Move nulls the source; the moved-to handle stages normally.
    ts::Guarded<int> target{ ts::Named{}, 0 };
    ts::Deferred<int> d{ target };

    ts::Recorder<int> rec;             // empty
    rec = d.recorder();                // late-bound
    rec.stage([](int& v) { v = 4; });
    d.commit().sync();
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 4);
}

// The destruction contract's compliant path: a synced commit_async settles the
// last commit, so the destructor neither fatals nor waits on the pipe.
void test_synced_commit_then_destroy()
{
    ts::Guarded<int> target{ ts::Named{}, 0 };
    {
        ts::Deferred<int> d{ target };
        auto rec = d.recorder();
        rec.stage([](int& v) { v += 5; });
        d.commit().sync();
    }   // dtor: last commit settled -> no fatal
    TS_CHECK(target.async([](const int& v) { return v; }).sync() == 5);
}

void test_drop_staged_is_fatal()
{
    TS_CHECK(ts::test::expect_death("deferred_drop_staged"));
}

void test_dtor_inflight_commit_is_fatal()
{
    TS_CHECK(ts::test::expect_death("deferred_dtor_inflight_commit"));
}

#if TS_SAFETY_CHECKS
// `commit()` from nested sub-work under an inherited write grant: not the holder, and the
// enqueued write would queue behind the parent's own hold - diagnosed at the call.
void test_commit_nested_grant_is_fatal()
{
    TS_CHECK(ts::test::expect_death("deferred_commit_nested_grant"));
}

// A `Recorder` outliving its `Deferred` would call `release_slot` on a destroyed journal;
// the journal destructor catches the outstanding slot and fatals first.
void test_recorder_outlives_journal_is_fatal()
{
    TS_CHECK(ts::test::expect_death("recorder_outlives_journal"));
}
#endif

void test_moved_from_recorder_stage_is_fatal()
{
    TS_CHECK(ts::test::expect_death("recorder_empty_stage"));
}

void test_empty_parallel_recorder_stage_is_fatal()
{
    TS_CHECK(ts::test::expect_death("parallel_recorder_empty_stage"));
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
    run("deferred: commit under a coroutine write guard", test_commit_under_coroutine_write_grant);
    run("deferred: commit as a graph node (re-run)", test_commit_in_graph_node);
    run("deferred: straggler rides the next commit", test_straggler_rides_next_commit);
    run("deferred: cancelled commit retains commands", test_cancelled_commit_retains_commands);
    run("deferred: discard drops staged commands", test_discard);
    run("deferred: move-only command capture", test_move_only_command);
    run("deferred: parallel recorder from parallel_for", test_parallel_recorder_from_parallel_for);
    run("deferred: parallel recorder overflow lane", test_parallel_recorder_overflow_lane);
    run("deferred: parallel recorder on versioned (replay exact)", test_parallel_recorder_on_versioned);
    run("deferred: released slot's commands survive", test_released_slot_commands_survive);
    run("deferred: slot reuse inherits apply position", test_slot_reuse_inherits_position);
    run("deferred: recorder churn does not grow the journal", test_recorder_churn_does_not_grow_journal);
    run("deferred: parallel recorder churn", test_parallel_recorder_churn);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "deferred: keeping recorders alive past max_slots is fatal", test_slot_overflow_is_fatal);
    run("deferred: late-bound recorder", test_late_bound_recorder);
    run("deferred: synced commit then destroy", test_synced_commit_then_destroy);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "deferred: destroy with staged commands is fatal", test_drop_staged_is_fatal);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "deferred: destroy with commit in flight is fatal", test_dtor_inflight_commit_is_fatal);
#if TS_SAFETY_CHECKS
    run("deferred: commit from nested inherited grant is fatal", test_commit_nested_grant_is_fatal);
    run("deferred: recorder outliving the journal is fatal", test_recorder_outlives_journal_is_fatal);
#endif
    run("deferred: stage on moved-from recorder is fatal", test_moved_from_recorder_stage_is_fatal);
    run("deferred: stage on empty parallel recorder is fatal", test_empty_parallel_recorder_stage_is_fatal);
}
