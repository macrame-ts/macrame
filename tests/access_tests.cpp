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

void test_death_no_context()    { TS_CHECK(ts::test::expect_death("access_no_context")); }
void test_death_ro_write()      { TS_CHECK(ts::test::expect_death("access_ro_write")); }
void test_death_wrong_instance(){ TS_CHECK(ts::test::expect_death("access_wrong_instance")); }

} // namespace

void run_access_tests()
{
    std::printf("\n[access] tests\n");
    run("grants logic", test_grants);
    run("scope nesting", test_scope_nesting);
    run("harness allows declared access", test_harness_allows);
    run("death: no context", test_death_no_context);
    run("death: read-only context + write", test_death_ro_write);
    run("death: wrong instance", test_death_wrong_instance);
}
