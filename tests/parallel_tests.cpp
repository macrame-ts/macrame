#include "parallel_tests.h"
#include "ts/parallel_for.h"
#include "harness.h"
#include "test_util.h"

#include <atomic>
#include <numeric>
#include <vector>

using ts::test::run;
using tests::wait_until;

namespace
{

// Every item runs exactly once, writing its own slot -- no data race, and the sum is right.
void check_each_item(ts::Balance balance)
{
    constexpr int n = 10000;
    std::vector<int> out(n, -1);
    ts::parallel_for(n, [&out](int i) { out[i] = i * 2; }, { .balance = balance });
    long long sum = 0;
    bool all = true;
    for (int i = 0; i < n; ++i)
    {
        all = all && (out[i] == i * 2);
        sum += out[i];
    }
    TS_CHECK(all);                                     // each slot written exactly by its item
    TS_CHECK(sum == static_cast<long long>(n) * (n - 1));   // 2 * sum(0..n-1)
}

void test_parallel_for_guided()     { check_each_item(ts::Balance::guided); }
void test_parallel_for_balanced()   { check_each_item(ts::Balance::balanced); }
void test_parallel_for_unbalanced() { check_each_item(ts::Balance::unbalanced); }

// Explicit concurrency, and a shared atomic accumulator (correct total across executors).
void test_parallel_for_concurrency()
{
    constexpr int n = 5000;
    std::atomic<long long> total{ 0 };
    ts::parallel_for(n, [&total](int i) { total.fetch_add(i, std::memory_order_relaxed); },
        { .concurrency = 4, .balance = ts::Balance::unbalanced });
    TS_CHECK(total.load() == static_cast<long long>(n) * (n - 1) / 2);
}

// Edge cases: n == 0 (no-op), n == 1 (one item), n < concurrency.
void test_parallel_for_edges()
{
    std::atomic<int> count{ 0 };
    ts::parallel_for(0, [&count](int) { count.fetch_add(1); });
    TS_CHECK(count.load() == 0);

    ts::parallel_for(1, [&count](int i) { count.fetch_add(1); TS_CHECK(i == 0); });
    TS_CHECK(count.load() == 1);

    count.store(0);
    ts::parallel_for(3, [&count](int) { count.fetch_add(1); }, { .concurrency = 16 });
    TS_CHECK(count.load() == 3);   // clamped to n executors, all items still run
}

// async: returns a Task<void> that completes when done; compose + sync.
void test_async_parallel_for()
{
    constexpr int n = 8000;
    std::vector<int> out(n, 0);
    ts::Task<void> t = ts::async_parallel_for(n, [&out](int i) { out[i] = i + 1; });
    t.sync();
    bool all = true;
    for (int i = 0; i < n; ++i)
        all = all && (out[i] == i + 1);
    TS_CHECK(all);

    ts::async_parallel_for(0, [](int) {}).sync();   // empty -> already complete
    TS_CHECK(true);
}

// Nested parallel_for must not deadlock: an outer parallel_for whose body runs an inner one.
// The outer occupies workers and each spawns more; caller-participation + wait-on-work-done
// drains it without needing the queued helpers to run.
void test_parallel_for_nested()
{
    constexpr int outer = 64, inner = 64;
    std::atomic<long long> total{ 0 };
    ts::parallel_for(outer, [&total](int)
    {
        ts::parallel_for(inner, [&total](int) { total.fetch_add(1, std::memory_order_relaxed); });
    });
    TS_CHECK(total.load() == static_cast<long long>(outer) * inner);
}

} // namespace

void run_parallel_tests()
{
    std::printf("\n[parallel_for] tests\n");
    run("parallel_for guided", test_parallel_for_guided);
    run("parallel_for balanced", test_parallel_for_balanced);
    run("parallel_for unbalanced", test_parallel_for_unbalanced);
    run("parallel_for concurrency", test_parallel_for_concurrency);
    run("parallel_for edges", test_parallel_for_edges);
    run("async_parallel_for", test_async_parallel_for);
    run("parallel_for nested", test_parallel_for_nested);
}
