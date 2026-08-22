#include "access_tests.h"
#include "ts/access.h"
#include "ts/fatal.h"
#include "ts/guarded.h"
#include "ts/parallel_for.h"
#include "ts/static_task_graph.h"
#include "harness.h"
#include "test_util.h"

#include <cstdio>

using ts::test::run;
using namespace ts::test;
using ts::Access;

namespace
{

void test_grants()
{
    int x = 0, y = 0, z = 0;
    ts::Access_context ctx;
    ctx.add(&x, Access::read_write);
    ctx.add(&y, Access::read_only);

    TS_CHECK(ctx.grants(&x, Access::read_only));    // read_write subsumes read_only
    TS_CHECK(ctx.grants(&x, Access::read_write));
    TS_CHECK(ctx.grants(&y, Access::read_only));
    TS_CHECK(!ctx.grants(&y, Access::read_write));  // read_only does not grant write
    TS_CHECK(!ctx.grants(&z, Access::read_only));   // unknown instance
}

void test_scope_nesting()
{
    TS_CHECK(ts::detail::access_load() == nullptr);

    ts::Access_context outer;
    {
        ts::Access_scope s1(outer);
        TS_CHECK(ts::detail::access_load() == &outer);

        ts::Access_context inner;
        {
            ts::Access_scope s2(inner);
            TS_CHECK(ts::detail::access_load() == &inner);
        }
        TS_CHECK(ts::detail::access_load() == &outer);   // restored
    }
    TS_CHECK(ts::detail::access_load() == nullptr);       // restored
}

void test_harness_allows()
{
    tests::Counter c;
    ts::Access_context ctx;
    ctx.add(&c, Access::read_write);
    ts::Access_scope scope(ctx);

    c.increment();          // read_write granted
    int v = c.value();      // read_only via read_write
    TS_CHECK(v == 1);
}

#if TS_SAFETY_CHECKS
// Grant-window staleness: an entry declared with an epoch source is valid while the
// epoch holds its captured value and reports `stale` (a distinct verdict, not `none`)
// once it moves - the unit-level contract behind the stale-inherited-grant fatal.
void test_epoch_staleness()
{
    using Grant = ts::Access_context::Grant;
    int x = 0;
    std::atomic<std::uint64_t> epoch{ 1 };   // odd: inside a write window

    ts::Access_context ctx;
    ctx.add(&x, Access::read_write, &epoch);
    TS_CHECK(ctx.check(&x, Access::read_write) == Grant::granted);
    TS_CHECK(ctx.check(&x, Access::read_only) == Grant::granted);

    epoch.fetch_add(1);   // window closes (release)
    TS_CHECK(ctx.check(&x, Access::read_write) == Grant::stale);
    TS_CHECK(ctx.check(&x, Access::read_only) == Grant::stale);
    TS_CHECK(!ctx.grants(&x, Access::read_write));   // stale is not granted

    int y = 0;
    TS_CHECK(ctx.check(&y, Access::read_only) == Grant::none);   // undeclared stays `none`
}

// A read-grant entry stays valid across other readers (no writer bump) and goes stale
// exactly when a writer acquires - and a null epoch source never goes stale.
void test_epoch_read_era_and_null()
{
    using Grant = ts::Access_context::Grant;
    int x = 0, z = 0;
    std::atomic<std::uint64_t> epoch{ 2 };   // even: reader era

    ts::Access_context ctx;
    ctx.add(&x, Access::read_only, &epoch);
    ctx.add(&z, Access::read_write);         // no epoch source

    TS_CHECK(ctx.check(&x, Access::read_only) == Grant::granted);   // readers come and go: no bump
    epoch.fetch_add(1);                       // a writer acquires
    TS_CHECK(ctx.check(&x, Access::read_only) == Grant::stale);
    TS_CHECK(ctx.check(&z, Access::read_write) == Grant::granted);  // null source: never stale
}
#endif

#if TS_SAFETY_CHECKS
// One fixed `TS_ENSURE` expansion site, so two calls exercise the once-per-site
// report filter against the every-occurrence counter.
bool trigger_ensure_site()
{
    return TS_ENSURE(false, "test: deliberate ensure failure (ignore)");
}

// The ensure facility: a passing expression is silent and yields true; a failing
// site counts every occurrence but reports once; no abort. (The debugger-break
// branch is untestable here - no debugger attached in CI.) `Expected_ensures`
// silences the deliberate failures (no F5 break) and consumes them on scope exit.
void test_ensure_facility()
{
    ts::test::Expected_ensures expect{ 2 };

    long long base = ts::ensure_failure_count();

    TS_CHECK(TS_ENSURE(1 + 1 == 2, "never fires"));
    TS_CHECK(ts::ensure_failure_count() == base);   // a pass costs nothing

    TS_CHECK(!trigger_ensure_site());               // yields the expression's value
    TS_CHECK(!trigger_ensure_site());               // same site: counts again, reports once
    TS_CHECK(ts::ensure_failure_count() == base + 2);
}

std::atomic<int> g_custom_handler_hits{ 0 };
void counting_ensure_handler(const char*) noexcept
{
    g_custom_handler_hits.fetch_add(1, std::memory_order_relaxed);
}

// The presentation hook: an installed handler replaces the default report (no
// stderr noise from this test), the counter still advances, and restoring
// returns the handler that was in place.
void test_ensure_handler_hook()
{
    long long base = ts::ensure_failure_count();

    ts::Ensure_handler prev = ts::set_ensure_handler(&counting_ensure_handler);
    TS_ENSURE(false, "test: handler hook");   // fresh site -> reports via the custom handler
    ts::Ensure_handler mine = ts::set_ensure_handler(prev);

    TS_CHECK(mine == &counting_ensure_handler);   // restore handed back what we installed
    TS_CHECK(g_custom_handler_hits.load() == 1);
    TS_CHECK(ts::ensure_failure_count() == base + 1);
    ts::test::consume_ensure_failures(1);
}
#endif

void test_death_no_context()    { TS_CHECK(ts::test::expect_death("access_no_context")); }
void test_death_ro_write()      { TS_CHECK(ts::test::expect_death("access_ro_write")); }
void test_death_wrong_instance(){ TS_CHECK(ts::test::expect_death("access_wrong_instance")); }
void test_death_context_overflow(){ TS_CHECK(ts::test::expect_death("access_context_overflow")); }
// A non-nested task launched from a graph node inherits the node's grant but outlives
// it; its guarded access after the node released must fault (the stale-grant fatal).
void test_death_stale_grant()   { TS_CHECK(ts::test::expect_death("stale_inherited_grant")); }
// A detached `ts::launch` inherits no grant, so a body touching the launcher's guarded data
// faults deterministically as undeclared access (docs/coroutine-first.md §2).
void test_death_detached_launch(){ TS_CHECK(ts::test::expect_death("detached_launch_undeclared")); }

// Companion to the detached-launch death test: the sanctioned form. `ts::parallel_for` helpers
// inherit the caller's grant (an Access_context snapshot), and the synchronous join keeps them
// strictly within the node's grant window - so they may touch the node's guarded data legally.
void test_nested_child_touches_guarded()
{
    ts::Guarded<tests::Counter> c{ ts::Named{ "c" } };
    ts::Static_task_graph g;
    g.add_node("writer", [](tests::Counter& k)
    {
        ts::parallel_for(1, [&k](int) { k.add(5); });   // grant-inheriting sub-work: legal guarded write
    }, c);
    g.compile();
    g.execute().sync();
    int seen = c.access([](const tests::Counter& k) { return k.value(); }).sync();
    TS_CHECK(seen == 5);
}

} // namespace

void run_access_tests()
{
    std::printf("\n[access] tests\n");
    run("grants logic", test_grants);
    run("scope nesting", test_scope_nesting);
    run("harness allows declared access", test_harness_allows);
#if TS_SAFETY_CHECKS
    run("epoch staleness verdicts", test_epoch_staleness);
    run("epoch read era + null source", test_epoch_read_era_and_null);
    run("ensure facility (once-per-site)", test_ensure_facility);
    run("ensure handler hook", test_ensure_handler_hook);
#endif
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "death: no context", test_death_no_context);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "death: read-only context + write", test_death_ro_write);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "death: wrong instance", test_death_wrong_instance);
    run("death: context overflow", test_death_context_overflow);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "death: stale inherited grant", test_death_stale_grant);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "death: detached launch undeclared access", test_death_detached_launch);
    run("nested child touches guarded (sanctioned)", test_nested_child_touches_guarded);
}
