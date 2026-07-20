#include "parallel_tests.h"
#include "ts/parallel_for.h"
#include "harness.h"
#include "test_util.h"

#include <atomic>
#include <memory>
#include <numeric>
#include <thread>
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

// A guarded store touched from inside parallel_for chunks, under the caller's write grant.
// Helpers run on other workers and must inherit the caller's `Access_context` (by-value
// snapshot in `Parallel_state`) or the harness faults. Many small items so helpers
// reliably claim some before the participating caller drains the range.
void test_parallel_for_inherits_grant()
{
    class Store
    {
    public:
        explicit Store(int n) : data_(n, 0) {}
        int size() const { TS_CHECK_ACCESS(); return static_cast<int>(data_.size()); }
        void set(int i, int v) { TS_CHECK_ACCESS(); data_[i] = v; }
        long long sum() const
        {
            TS_CHECK_ACCESS();
            return std::accumulate(data_.begin(), data_.end(), 0ll);
        }
    private:
        std::vector<int> data_;
    };

    constexpr int n = 20000;
    ts::Guarded<Store> store{ n };
    long long total = store.access([](Store& s)
    {
        ts::parallel_for(s.size(), [&s](int i) { s.set(i, 1); });
        return s.sum();
    }).sync();
    TS_CHECK(total == n);
}

// Correctness is priority-independent: every item runs exactly once at each explicit class,
// for both the blocking and the async form.
void check_sum_at_priority(ts::Priority p)
{
    constexpr int n = 4000;
    std::atomic<long long> total{ 0 };
    ts::parallel_for(n, [&total](int i) { total.fetch_add(i, std::memory_order_relaxed); },
        { .priority = p });
    TS_CHECK(total.load() == static_cast<long long>(n) * (n - 1) / 2);
}

void test_parallel_for_priorities()
{
    check_sum_at_priority(ts::Priority::high);
    check_sum_at_priority(ts::Priority::normal);
    check_sum_at_priority(ts::Priority::low);

    constexpr int n = 4000;
    std::atomic<long long> total{ 0 };
    ts::async_parallel_for(n, [&total](int i) { total.fetch_add(i, std::memory_order_relaxed); },
        { .priority = ts::Priority::high }).sync();
    TS_CHECK(total.load() == static_cast<long long>(n) * (n - 1) / 2);
}

// The resolution rule (`detail::resolved_priority` -- the exact value every helper submit
// gets): unset inherits the calling task's priority via the `current_task` TLS, `normal`
// outside a task; an explicit option always wins.
void test_parallel_for_priority_inheritance()
{
    // Not inside a task: inherit falls back to `normal`; explicit overrides.
    TS_CHECK(ts::detail::resolved_priority({}) == ts::Priority::normal);
    TS_CHECK(ts::detail::resolved_priority(ts::Priority::high) == ts::Priority::high);

    // Inside a launched task, unset resolves to the launcher's priority.
    const ts::Priority classes[] = { ts::Priority::high, ts::Priority::normal, ts::Priority::low };
    for (ts::Priority p : classes)
    {
        std::atomic<int> seen{ -1 };
        ts::launch([&seen]()
        {
            seen.store(static_cast<int>(ts::detail::resolved_priority({})));
        }, { .priority = p }).sync();
        TS_CHECK(seen.load() == static_cast<int>(p));
    }

    // Explicit beats inheritance inside a task too.
    std::atomic<int> forced{ -1 };
    ts::launch([&forced]()
    {
        forced.store(static_cast<int>(ts::detail::resolved_priority(ts::Priority::low)));
    }, { .priority = ts::Priority::high }).sync();
    TS_CHECK(forced.load() == static_cast<int>(ts::Priority::low));
}

// A raw submission that parks its worker until released (the held-worker observation pattern).
struct Hold
{
    std::atomic<int>* entered;
    std::atomic<int>* exited;
    std::atomic<bool> go{ false };
};

void hold_worker(void* p)
{
    auto* h = static_cast<Hold*>(p);
    h->entered->fetch_add(1);
    while (!h->go.load(std::memory_order_acquire))
        std::this_thread::yield();
    h->exited->fetch_add(1);
}

void set_flag(void* p)
{
    static_cast<std::atomic<bool>*>(p)->store(true, std::memory_order_release);
}

// Deterministic dispatch-order observation on the shared default scheduler: hold every
// worker, free exactly one and feed it a `low` dummy (popping a low task resets that
// worker's starvation-valve counter, so the order below is independent of prior suite
// history), catch it again, queue one single-helper `async_parallel_for` per class, then
// release it alone -- it drains the global queues sequentially, in strict class order,
// while the other workers stay held (no concurrent pops to interleave).
void test_parallel_for_priority_order()
{
    ts::Scheduler& sched = ts::default_scheduler();
    const int workers = sched.worker_count();
    std::atomic<int> entered{ 0 }, exited{ 0 };

    std::vector<std::unique_ptr<Hold>> holds;
    for (int i = 0; i < workers + 1; ++i)
        holds.push_back(std::unique_ptr<Hold>(new Hold{ &entered, &exited }));

    for (int i = 0; i < workers; ++i)
        sched.submit(&hold_worker, holds[static_cast<std::size_t>(i)].get());
    wait_until([&entered, workers] { return entered.load() == workers; });

    // Free one worker; it pops the dummy (via the low path or the valve -- both reset the
    // valve counter) and idles.
    holds[0]->go.store(true, std::memory_order_release);
    std::atomic<bool> low_ran{ false };
    sched.submit(&set_flag, &low_ran, ts::Priority::low);
    wait_until([&low_ran] { return low_ran.load(); });

    // Catch the freed worker so the recorders below queue up race-free.
    sched.submit(&hold_worker, holds[static_cast<std::size_t>(workers)].get());
    wait_until([&entered, workers] { return entered.load() == workers + 1; });

    // One single-item, single-helper loop per class. The middle one leaves the priority
    // unset -- inherited from a non-task caller it must resolve to `normal` and land
    // between the explicit `high` and `low` (end-to-end inherit check).
    std::atomic<int> idx{ 0 };
    int order[3] = { -1, -1, -1 };
    auto record = [&idx, &order](int tag)
    {
        return [&idx, &order, tag](int) { order[idx.fetch_add(1)] = tag; };
    };
    ts::Task<void> hi = ts::async_parallel_for(1, record(0), { .priority = ts::Priority::high });
    ts::Task<void> mid = ts::async_parallel_for(1, record(1));
    ts::Task<void> lo = ts::async_parallel_for(1, record(2), { .priority = ts::Priority::low });

    holds[static_cast<std::size_t>(workers)]->go.store(true, std::memory_order_release);
    hi.sync();
    mid.sync();
    lo.sync();
    TS_CHECK(idx.load() == 3);
    TS_CHECK(order[0] == 0 && order[1] == 1 && order[2] == 2);

    // Release the rest and wait for every hold body to exit before the stack state unwinds.
    for (int i = 1; i < workers; ++i)
        holds[static_cast<std::size_t>(i)]->go.store(true, std::memory_order_release);
    wait_until([&exited, workers] { return exited.load() == workers + 1; });
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
    run("parallel_for inherits grant", test_parallel_for_inherits_grant);
    run("parallel_for priorities", test_parallel_for_priorities);
    run("parallel_for priority inheritance", test_parallel_for_priority_inheritance);
    run("parallel_for priority order", test_parallel_for_priority_order);
}
