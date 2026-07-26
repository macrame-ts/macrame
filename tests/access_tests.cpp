#include "access_tests.h"
#include "ts/access.h"
#include "harness.h"
#include "test_util.h"

#include <cstdio>

using ts::test::run;
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
    TS_CHECK(ts::detail::current_access == nullptr);

    ts::Access_context outer;
    {
        ts::Access_scope s1(outer);
        TS_CHECK(ts::detail::current_access == &outer);

        ts::Access_context inner;
        {
            ts::Access_scope s2(inner);
            TS_CHECK(ts::detail::current_access == &inner);
        }
        TS_CHECK(ts::detail::current_access == &outer);   // restored
    }
    TS_CHECK(ts::detail::current_access == nullptr);       // restored
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

// Grant-window staleness: an entry declared with an epoch source is valid while the
// epoch holds its captured value and reports `stale` (a distinct verdict, not `none`)
// once it moves -- the unit-level contract behind the stale-inherited-grant fatal.
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
// exactly when a writer acquires -- and a null epoch source never goes stale.
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

void test_death_no_context()    { TS_CHECK(ts::test::expect_death("access_no_context")); }
void test_death_ro_write()      { TS_CHECK(ts::test::expect_death("access_ro_write")); }
void test_death_wrong_instance(){ TS_CHECK(ts::test::expect_death("access_wrong_instance")); }
void test_death_context_overflow(){ TS_CHECK(ts::test::expect_death("access_context_overflow")); }
// A non-nested task launched from a graph node inherits the node's grant but outlives
// it; its guarded access after the node released must fault (the stale-grant fatal).
void test_death_stale_grant()   { TS_CHECK(ts::test::expect_death("stale_inherited_grant")); }

} // namespace

void run_access_tests()
{
    std::printf("\n[access] tests\n");
    run("grants logic", test_grants);
    run("scope nesting", test_scope_nesting);
    run("harness allows declared access", test_harness_allows);
    run("epoch staleness verdicts", test_epoch_staleness);
    run("epoch read era + null source", test_epoch_read_era_and_null);
    run("death: no context", test_death_no_context);
    run("death: read-only context + write", test_death_ro_write);
    run("death: wrong instance", test_death_wrong_instance);
    run("death: context overflow", test_death_context_overflow);
    run("death: stale inherited grant", test_death_stale_grant);
}
