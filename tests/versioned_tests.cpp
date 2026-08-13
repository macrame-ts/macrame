#include "versioned_tests.h"
#include "ts/versioned.h"
#include "ts/static_task_graph.h"
#include "harness.h"
#include "test_util.h"

#include <atomic>
#include <cstddef>
#include <thread>
#include <type_traits>
#include <vector>

using ts::test::run;
using namespace ts::test;

namespace
{

// --- compile-time contract --------------------------------------------------

static_assert(!std::is_copy_constructible_v<ts::Versioned<int>>);
static_assert(std::is_same_v<
    decltype(std::declval<ts::Versioned<int>&>().publish()), ts::Task<void>>);

// --- basics -------------------------------------------------------------------

void test_initial_read()
{
    ts::Versioned<int> v{ ts::Named{} };
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 0);
}

void test_stage_publish_read()
{
    ts::Versioned<int> v{ ts::Named{} };
    auto rec = v.recorder();
    rec.stage([](int& x) { x = 42; });
    v.publish().sync();
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 42);
}

void test_stability_before_publish()
{
    // Staged writes are invisible until the publish.
    ts::Versioned<int> v{ ts::Named{} };
    auto rec = v.recorder();
    rec.stage([](int& x) { x = 42; });
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 0);
    v.publish().sync();
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 42);
}

void test_empty_publish_noop()
{
    ts::Versioned<int> v{ ts::Named{} };
    auto rec = v.recorder();
    rec.stage([](int& x) { x = 5; });
    v.publish().sync();
    v.publish().sync();   // nothing staged: readers keep the current version
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 5);
}

// --- the replica invariant (the load-bearing test) -----------------------------

void test_replay_resync_invariant()
{
    // Deltas accumulate across publishes. If the shadow were not resynced after
    // each swap, publish N+1 would apply its delta to a version N-1 shadow and
    // readers would observe dropped history.
    ts::Versioned<int> v{ ts::Named{} };   // Resync::replay
    auto rec = v.recorder();

    rec.stage([](int& x) { x += 1; });
    v.publish().sync();
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 1);

    rec.stage([](int& x) { x += 2; });
    v.publish().sync();
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 3);   // 2 if the shadow was stale

    rec.stage([](int& x) { x *= 2; });   // read-modify-write: sees identical pre-state on both replicas
    v.publish().sync();
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 6);
}

void test_copy_resync_invariant()
{
    ts::Versioned<int> v{ ts::Named{}, ts::Resync::copy };
    auto rec = v.recorder();
    rec.stage([](int& x) { x += 1; });
    v.publish().sync();
    rec.stage([](int& x) { x += 2; });
    v.publish().sync();
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 3);
}

void test_copy_custom_fn()
{
    ts::Versioned<int> v{ ts::Named{}, ts::Resync::copy };
    std::atomic<int> copies{ 0 };
    v.set_copy([&copies](int& dst, const int& src) { dst = src; copies.fetch_add(1); });

    auto rec = v.recorder();
    rec.stage([](int& x) { x = 11; });
    v.publish().sync();
    rec.stage([](int& x) { x += 1; });
    v.publish().sync();
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 12);
    // `publish().sync()` returns at the swap; the copy runs in the trailing
    // resync. An empty publish is a resync fence (its phase 1 gates on the
    // previous shadow_ready), so sync it before counting.
    v.publish().sync();
    TS_CHECK(copies.load() == 2);
}

void test_overwrite_policy()
{
    // Contract: every version's staged writes fully overwrite the state, so the
    // (stale) shadow contents never matter.
    ts::Versioned<std::vector<int>> v{ ts::Named{}, ts::Resync::overwrite };
    auto rec = v.recorder();

    rec.stage([](std::vector<int>& x) { x = { 1, 2 }; });
    v.publish().sync();
    rec.stage([](std::vector<int>& x) { x = { 3, 4, 5 }; });
    v.publish().sync();

    auto out = v.read([](const std::vector<int>& x) { return x; }).sync();
    TS_CHECK((out == std::vector<int>{ 3, 4, 5 }));
}

void test_divergence_check_passes_for_deterministic()
{
    ts::Versioned<int> v{ ts::Named{} };
    v.set_divergence_check([](const int& x) { return static_cast<std::size_t>(x); });
    auto rec = v.recorder();
    for (int i = 0; i < 5; ++i)
    {
        rec.stage([i](int& x) { x += i; });
        v.publish().sync();   // hash compare after every replay resync - must not fatal
    }
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 0 + 1 + 2 + 3 + 4);
}

// --- ordering ------------------------------------------------------------------

void test_multi_recorder_order()
{
    ts::Versioned<std::vector<int>> v{ ts::Named{} };
    auto rec1 = v.recorder();
    auto rec2 = v.recorder();
    rec2.stage([](std::vector<int>& x) { x.push_back(2); });
    rec1.stage([](std::vector<int>& x) { x.push_back(1); });
    v.publish().sync();
    auto out = v.read([](const std::vector<int>& x) { return x; }).sync();
    TS_CHECK((out == std::vector<int>{ 1, 2 }));
}

void test_chained_publishes_apply_exactly_once()
{
    // Publishes fired back-to-back without syncing chain internally (phase 1 of
    // each gates on the previous resync); every staged command lands in exactly
    // one cut.
    ts::Versioned<int> v{ ts::Named{} };
    auto rec = v.recorder();
    std::vector<ts::Task<void>> pubs;
    for (int i = 0; i < 20; ++i)
    {
        rec.stage([](int& x) { x += 1; });
        pubs.push_back(v.publish());
    }
    for (auto& p : pubs)
        p.sync();
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 20);
}

void test_cancelled_publish_retains_commands()
{
    ts::Cancellation_source src;
    src.request_cancel();

    ts::Versioned<int> v{ ts::Named{} };
    auto rec = v.recorder();
    rec.stage([](int& x) { x = 7; });

    // A cancelled token skips the version step; the returned task is a phase
    // gate, so it completes (unlike a cancelled pipe job) and the commands stay
    // staged for the next publish.
    ts::Task<void> p = v.publish({ .token = src.token() });
    p.sync();
    TS_CHECK(!p.is_cancelled());
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 0);

    v.publish().sync();
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 7);
}

void test_reader_overlaps_resync()
{
    // The phase-split's point: after the swap, the resync runs as a pipe read
    // job, so a reader of the new version is admitted while the resync runs.
    // The divergence hook doubles as the probe: its first call parks inside the
    // resync until the reader arrives - the gate is met only if both were in
    // flight at once.
    tests::Parallel_gate gate{ 2 };
    std::atomic<int> hash_calls{ 0 };

    ts::Versioned<int> v{ ts::Named{} };
    v.set_divergence_check([&gate, &hash_calls](const int& x)
    {
        if (hash_calls.fetch_add(1) == 0)
            gate.arrive();   // parks the resync job (bounded)
        return static_cast<std::size_t>(x);
    });

    auto rec = v.recorder();
    rec.stage([](int& x) { x = 5; });
    v.publish().sync();   // returns at the swap; the resync job is queued/running

    int seen = v.read([&gate](const int& x)
    {
        gate.arrive();
        return x;
    }).sync();

    TS_CHECK(gate.met());   // reader ran concurrently with the resync
    TS_CHECK(seen == 5);    // and saw the new version
}

// --- concurrency -----------------------------------------------------------------

void test_concurrent_readers_and_publishes()
{
    constexpr int publishes = 50;
    constexpr int reader_threads = 4;

    ts::Versioned<int> v{ ts::Named{} };
    std::atomic<bool> stop{ false };
    std::atomic<int> bad{ 0 };

    std::vector<std::jthread> readers;
    for (int t = 0; t < reader_threads; ++t)
    {
        readers.emplace_back([&v, &stop, &bad]
        {
            while (!stop.load())
            {
                int x = v.read([](const int& val) { return val; }).sync();
                // Every observed value must be a published version: 0..publishes.
                if (x < 0 || x > publishes)
                    bad.fetch_add(1);
            }
        });
    }

    auto rec = v.recorder();
    for (int i = 0; i < publishes; ++i)
    {
        rec.stage([](int& x) { x += 1; });
        v.publish().sync();
    }
    stop.store(true);
    readers.clear();

    TS_CHECK(bad.load() == 0);
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == publishes);
}

// --- graph integration ------------------------------------------------------------

void test_graph_stale_then_fresh()
{
    // One frame: `before` (declared before the flip) reads version N-1, `after`
    // (ordered after the flip) reads version N. Run twice to prove re-arming.
    ts::Versioned<int> v{ ts::Named{} };
    ts::Guarded<std::vector<int>> seen_before{ ts::Named{ "seen_before" } }, seen_after{ ts::Named{ "seen_after" } };

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](const int& x, std::vector<int>& log) { log.push_back(x); },
               v.state(), seen_before);   // "before": declared before the flip -> derived read->write edge
    auto producer = g.add_node(ts::Named{}, [rec = v.recorder()](std::vector<int>&) mutable
    {
        rec.stage([](int& x) { x += 1; });
    }, seen_after);   // touches seen_after only to have some declared access
    auto flip = g.add_node(ts::Named{}, ts::publish_fn(v), v.state());
    flip.after(producer);
    auto after = g.add_node(ts::Named{}, [](const int& x, std::vector<int>& log) { log.push_back(x); },
                            v.state(), seen_after);
    after.after(flip);
    g.compile();

    g.execute().sync();
    g.execute().sync();

    auto b = seen_before.async([](const std::vector<int>& l) { return l; }).sync();
    auto a = seen_after.async([](const std::vector<int>& l) { return l; }).sync();
    TS_CHECK((b == std::vector<int>{ 0, 1 }));   // stale: version N-1 each frame
    TS_CHECK((a == std::vector<int>{ 1, 2 }));   // fresh: version N each frame
}

// --- fatal paths --------------------------------------------------------------------

void test_publish_sync_then_graph_flip()
{
    // The legal mixed pattern: a synced dynamic publish followed by a graph
    // flip. sync() returning guarantees the resync job is already ON the pipe
    // (enqueued before the phase gate triggers), so the flip's acquire orders
    // behind it and the flip-entry enforcement check passes deterministically.
    // 200 iterations amplify the timing window.
    ts::Versioned<int> v{ ts::Named{} };
    auto rec = v.recorder();

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, ts::publish_fn(v), v.state());
    g.compile();

    constexpr int rounds = 200;
    for (int i = 0; i < rounds; ++i)
    {
        rec.stage([](int& x) { x += 1; });
        v.publish().sync();       // dynamic publish...
        rec.stage([](int& x) { x += 1; });
        g.execute().sync();       // ...then a graph flip: must neither fatal nor lose a write
    }
    TS_CHECK(v.read([](const int& x) { return x; }).sync() == 2 * rounds);
}

// The legal single-publisher direction (the fatal reverse is versioned_mixed_publish):
// a dynamic fire-and-forget publish() arriving while a graph flip is still unresolved
// chains behind the flip - no fatal - and its write lands after the flip's version.
void test_dynamic_publish_chains_behind_flip()
{
#if TS_SAFETY_CHECKS
    long long ensure_base = ts::ensure_failure_count();
#endif

    ts::Versioned<int> v{ ts::Named{} };
    std::atomic<int> apply_count{ 0 };
    std::atomic<bool> release{ false };

    // The flip publishes version "100". The command also blocks its own resync (the
    // second application) so the flip stays unresolved - chain_ not-done - giving a
    // deterministic mid-flip window; phase 1 (first application) does not block.
    {
        auto rec = v.recorder();
        rec.stage([&apply_count, &release](int& x)
        {
            x = 100;
            if (apply_count.fetch_add(1, std::memory_order_acq_rel) == 1)
            {
                while (!release.load(std::memory_order_acquire))
                    std::this_thread::yield();
            }
        });
    }

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, ts::publish_fn(v), v.state());
    g.compile();
    g.execute().sync();   // flip runs phases 1-2 and installs its shadow_ready; its resync
                          // is now blocked -> chain_ stays not-done

    // In that window a dynamic publish must chain behind the flip, not fatal.
    ts::Task<void> dyn;
    {
        auto rec = v.recorder();
        rec.stage([](int& x) { x += 7; });
        dyn = v.publish();   // prev = the flip's (not-done) shadow_ready -> chains behind it
    }

    release.store(true, std::memory_order_release);   // let the flip's resync, then the dynamic, finish
    dyn.sync();

    int final = v.read([](const int& x) { return x; }).sync();
    // 107 = flip set 100, then the chained dynamic added 7. A raced/reordered publish
    // (dynamic before flip) would leave 100 (the flip overwriting the +7); a dropped
    // write would leave 100 or 7. Only the correct chained order yields 107.
    TS_CHECK(final == 107);
#if TS_SAFETY_CHECKS
    TS_CHECK(ts::ensure_failure_count() == ensure_base);   // legal path: no diagnostic fired
#endif
}

void test_mixed_publish_race_is_fatal()
{
    TS_CHECK(ts::test::expect_death("versioned_mixed_publish"));
}

void test_divergence_is_fatal()
{
    TS_CHECK(ts::test::expect_death("versioned_divergence"));
}

void test_wrong_front_is_fatal()
{
    TS_CHECK(ts::test::expect_death("versioned_wrong_front"));
}

void test_drop_staged_is_fatal()
{
    TS_CHECK(ts::test::expect_death("versioned_drop_staged"));
}

void test_dtor_inflight_publish_is_fatal()
{
    TS_CHECK(ts::test::expect_death("versioned_dtor_inflight_publish"));
}

// A synced publish followed by destruction is clean: no fatal, and the published
// value is visible. (The resync tail may still be draining; the destructor waits it
// out silently rather than faulting.)
void test_synced_publish_then_destroy()
{
    int seen = -1;
    {
        ts::Versioned<int> v{ ts::Named{} };
        auto rec = v.recorder();
        rec.stage([](int& x) { x = 7; });
        v.publish().sync();
        seen = v.read([](const int& x) { return x; }).sync();
    }   // ~Versioned with the publish resolved -> no fatal
    TS_CHECK(seen == 7);
}

} // namespace

void run_versioned_tests()
{
    run("versioned: initial read", test_initial_read);
    run("versioned: stage + publish + read", test_stage_publish_read);
    run("versioned: staged writes invisible until publish", test_stability_before_publish);
    run("versioned: empty publish is a no-op", test_empty_publish_noop);
    run("versioned: replay resync keeps replicas converged", test_replay_resync_invariant);
    run("versioned: copy resync keeps replicas converged", test_copy_resync_invariant);
    run("versioned: custom copy fn", test_copy_custom_fn);
    run("versioned: overwrite policy", test_overwrite_policy);
    run("versioned: divergence check passes when deterministic", test_divergence_check_passes_for_deterministic);
    run("versioned: multi-recorder apply order", test_multi_recorder_order);
    run("versioned: chained publishes apply exactly once", test_chained_publishes_apply_exactly_once);
    run("versioned: cancelled publish retains commands", test_cancelled_publish_retains_commands);
    run_if(with_harness, "TS_SAFETY_CHECKS=0 (the probe is the divergence hook)", "versioned: reader overlaps the resync", test_reader_overlaps_resync);
    run("versioned: concurrent readers during publishes", test_concurrent_readers_and_publishes);
    run("versioned: graph - stale before flip, fresh after", test_graph_stale_then_fresh);
    run("versioned: synced publish then graph flip is legal", test_publish_sync_then_graph_flip);
    run("versioned: dynamic publish chains behind an in-flight flip", test_dynamic_publish_chains_behind_flip);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "versioned: flip catching an unresolved publish is fatal", test_mixed_publish_race_is_fatal);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "versioned: nondeterministic replay is fatal", test_divergence_is_fatal);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "versioned: publish_into wrong instance is fatal", test_wrong_front_is_fatal);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "versioned: destroy with staged commands is fatal", test_drop_staged_is_fatal);
    run("versioned: synced publish then destroy is clean", test_synced_publish_then_destroy);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "versioned: destroy with a publish in flight is fatal", test_dtor_inflight_publish_is_fatal);
}
