#include "scheduler_scope.h"
#include "parallel_tests.h"
#include "ts/parallel_for.h"
#include "harness.h"
#include "test_util.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>

using ts::test::run;
using ts::test::run_if;
using ts::test::with_rule_in_task_sync;
using tests::wait_until;

namespace
{

// Every item runs exactly once, writing its own slot - no data race, and the sum is right.
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
        { .max_workers = 4, .balance = ts::Balance::unbalanced });
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
    ts::parallel_for(3, [&count](int) { count.fetch_add(1); }, { .max_workers = 16 });
    TS_CHECK(count.load() == 3);   // clamped to n executors, all items still run
}

// async: returns a Task<void> that completes when done; compose + sync.
void test_parallel_for_async()
{
    constexpr int n = 8000;
    std::vector<int> out(n, 0);
    ts::Task<void> t = ts::parallel_for_async(n, [&out](int i) { out[i] = i + 1; });
    t.sync();
    bool all = true;
    for (int i = 0; i < n; ++i)
        all = all && (out[i] == i + 1);
    TS_CHECK(all);

    ts::parallel_for_async(0, [](int) {}).sync();   // empty -> already complete
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
    ts::Guarded<Store> store{ ts::Named{}, n };
    long long total = store.access([](Store& s)
    {
        ts::parallel_for(s.size(), [&s](int i) { s.set(i, 1); });
        return s.sum();
    }).sync();
    TS_CHECK(total == n);
}

// A helper chunk inherits the caller's relaxation along with the caller's grant. Without
// that, enforcement depends on which chunk a thread happened to claim: the caller's own
// share runs inline (so it is relaxed) while helpers on other workers are not.
void test_parallel_for_inherits_relaxation()
{
    constexpr int n = 20000;
    std::vector<char> saw_relaxed(n, 0);
    std::vector<std::thread::id> ran_on(n);

    {
        ts::Relaxed_scope relaxed{ ts::Rule::in_task_sync };
        ts::parallel_for(n, [&](int i)
        {
            saw_relaxed[i] = ts::rule_relaxed(ts::Rule::in_task_sync) ? char(1) : char(0);
            ran_on[i] = std::this_thread::get_id();
        });
    }

    int relaxed_count = 0;
    for (int i = 0; i < n; ++i)
        relaxed_count += saw_relaxed[i];
    TS_CHECK(relaxed_count == n);

    // The assertion above only bites when helpers actually ran; report it so a run where
    // the caller claimed every chunk cannot pass as coverage it did not provide.
    std::vector<std::thread::id> distinct(ran_on);
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    if (distinct.size() < 2 && ts::global_scheduler().worker_count() > 1)
        std::printf("    (note: caller claimed every chunk; helper path not exercised)\n");

    // Leaving the scope restores the caller, and no helper leaked its install onto a worker.
    TS_CHECK(!ts::rule_relaxed(ts::Rule::in_task_sync));
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
    ts::parallel_for_async(n, [&total](int i) { total.fetch_add(i, std::memory_order_relaxed); },
        { .priority = ts::Priority::high }).sync();
    TS_CHECK(total.load() == static_cast<long long>(n) * (n - 1) / 2);
}

// The resolution rule (`detail::resolved_priority` - the exact value every helper submit
// gets): unset inherits the calling task's priority via the `Current_task` TLS, `normal`
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
// history), catch it again, queue one single-helper `parallel_for_async` per class, then
// release it alone - it drains the global queues sequentially, in strict class order,
// while the other workers stay held (no concurrent pops to interleave).
void test_parallel_for_priority_order()
{
    ts::Scheduler& sched = ts::global_scheduler();
    const int workers = sched.worker_count();
    std::atomic<int> entered{ 0 }, exited{ 0 };

    std::vector<std::unique_ptr<Hold>> holds;
    for (int i = 0; i < workers + 1; ++i)
        holds.push_back(std::unique_ptr<Hold>(new Hold{ &entered, &exited }));

    for (int i = 0; i < workers; ++i)
        sched.submit(&hold_worker, holds[static_cast<std::size_t>(i)].get());
    wait_until([&entered, workers] { return entered.load() == workers; });

    // Free one worker; it pops the dummy (via the low path or the valve - both reset the
    // valve counter) and idles.
    holds[0]->go.store(true, std::memory_order_release);
    std::atomic<bool> low_ran{ false };
    sched.submit(&set_flag, &low_ran, ts::Priority::low);
    wait_until([&low_ran] { return low_ran.load(); });

    // Catch the freed worker so the recorders below queue up race-free.
    sched.submit(&hold_worker, holds[static_cast<std::size_t>(workers)].get());
    wait_until([&entered, workers] { return entered.load() == workers + 1; });

    // One single-item, single-helper loop per class. The middle one leaves the priority
    // unset - inherited from a non-task caller it must resolve to `normal` and land
    // between the explicit `high` and `low` (end-to-end inherit check).
    std::atomic<int> idx{ 0 };
    int order[3] = { -1, -1, -1 };
    auto record = [&idx, &order](int tag)
    {
        return [&idx, &order, tag](int) { order[idx.fetch_add(1)] = tag; };
    };
    ts::Task<void> hi = ts::parallel_for_async(1, record(0), { .priority = ts::Priority::high });
    ts::Task<void> mid = ts::parallel_for_async(1, record(1));
    ts::Task<void> lo = ts::parallel_for_async(1, record(2), { .priority = ts::Priority::low });

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

// `Parallel_options::token` on the async form: hold every worker so the helpers queue behind
// the holds, request cancellation while they are all still queued, then release. Every helper
// then finds the token requested on its first claim, skips the whole range, and the task settles
// cancelled with no chunk body ever run. Deterministic: nothing can claim a chunk before the
// request lands, because nothing is running.
void test_parallel_for_async_cancel()
{
    ts::Scheduler& sched = ts::global_scheduler();
    const int workers = sched.worker_count();
    std::atomic<int> entered{ 0 }, exited{ 0 };

    std::vector<std::unique_ptr<Hold>> holds;
    for (int i = 0; i < workers; ++i)
        holds.push_back(std::unique_ptr<Hold>(new Hold{ &entered, &exited }));
    for (int i = 0; i < workers; ++i)
        sched.submit(&hold_worker, holds[static_cast<std::size_t>(i)].get());
    wait_until([&entered, workers] { return entered.load() == workers; });

    std::atomic<int> ran{ 0 };
    ts::Cancellation_source source;
    ts::Task<void> loop = ts::parallel_for_async(1000, [&ran](int) { ran.fetch_add(1, std::memory_order_relaxed); },
        { .balance = ts::Balance::unbalanced, .token = source.token() });
    TS_CHECK(!loop.is_done());
    source.request_cancel();

    for (int i = 0; i < workers; ++i)
        holds[static_cast<std::size_t>(i)]->go.store(true, std::memory_order_release);
    loop.sync();   // a cancelled void task unblocks normally
    TS_CHECK(loop.is_done());
    TS_CHECK(loop.is_cancelled());
    TS_CHECK(ran.load() == 0);
    wait_until([&exited, workers] { return exited.load() == workers; });

    // The blocking form ignores the field: the same requested token, every item still runs.
    std::atomic<int> blocking_ran{ 0 };
    ts::parallel_for(64, [&blocking_ran](int) { blocking_ran.fetch_add(1, std::memory_order_relaxed); },
        { .token = source.token() });
    TS_CHECK(blocking_ran.load() == 64);

    // An unrequested token leaves the async form as it was.
    ts::Cancellation_source idle;
    std::atomic<int> all{ 0 };
    ts::Task<void> full = ts::parallel_for_async(64, [&all](int) { all.fetch_add(1, std::memory_order_relaxed); },
        { .token = idle.token() });
    full.sync();
    TS_CHECK(!full.is_cancelled());
    TS_CHECK(all.load() == 64);
}

// A parallel_for launched from a task fans out on the running pool: reconfigure the one global
// scheduler to 2 workers, run parallel_for inside a task on it, and assert every executor that
// touched an item reports a worker index in [0, 2). (With the single-global collapse there is
// no separate "current" pool to route to; this pins that fan-out stays on the running pool's
// workers and inherits their indices.)
constexpr int pf_sched_n = 512;
std::vector<int> pf_sched_worker;      // static storage so the fn-ptr task can reach it
std::atomic<bool> pf_sched_done{ false };

void test_parallel_for_current_scheduler()
{
    ts::Scheduler_scope scope{ { .num_workers = 2 } };
    ts::Scheduler& dedicated = ts::global_scheduler();
    pf_sched_worker.assign(pf_sched_n, -2);
    pf_sched_done.store(false);

    dedicated.submit([](void*)
    {
        ts::parallel_for(pf_sched_n, [](int i)
        {
            pf_sched_worker[static_cast<size_t>(i)] = ts::current_worker_index();
        });
        pf_sched_done.store(true, std::memory_order_release);
    }, nullptr);

    wait_until([] { return pf_sched_done.load(std::memory_order_acquire); });

    int max_w = -1;
    bool all_valid = true;
    for (int w : pf_sched_worker)
    {
        if (w < 0)               // -2 unset or -1 non-worker: every item must run on a worker
            all_valid = false;
        max_w = std::max(max_w, w);
    }
    TS_CHECK(all_valid);         // every item ran on a worker of the dedicated pool
    TS_CHECK(max_w < 2);         // ...and only its 2 workers (not the wider default pool)
}

// --- parallel_for_colored (interaction coloring) -------------------------------------

// Each item of each band runs once per round: rounds passes over the bands.
void test_colored_counts()
{
    constexpr int n = 6;
    std::vector<std::atomic<int>> hits(n);   // C++20 value-inits atomics to 0
    std::vector<std::vector<int>> bands = { { 0, 2, 4 }, { 1, 3 }, { 5 } };
    ts::parallel_for_colored(bands, 3, [&hits](int i) { hits[i].fetch_add(1, std::memory_order_relaxed); },
        { .max_workers = 4 });
    for (int i = 0; i < n; ++i)
        TS_CHECK(hits[i].load() == 3);
}

// A 1D relaxation: values along a line, constraints (ci, ci+1) 2-colored even/odd. Each band's
// constraints touch disjoint particles, so a band's result is chunking-independent; bands run
// sequentially, so the field depends on band order but NOT on worker count. Same bands + rounds
// must give a bit-identical field at any concurrency - the property the design exists for.
std::vector<float> relax_chain(int m, int rounds, int conc)
{
    std::vector<float> v(static_cast<std::size_t>(m));
    for (int i = 0; i < m; ++i)
        v[i] = static_cast<float>(i);   // deterministic init
    std::vector<std::vector<int>> bands(2);
    for (int ci = 0; ci + 1 < m; ++ci)
        bands[ci & 1].push_back(ci);    // ci connects particles ci and ci+1
    ts::parallel_for_colored(bands, rounds, [&v](int ci)
    {
        float mid = 0.5f * (v[ci] + v[ci + 1]);
        v[ci] = mid;
        v[ci + 1] = mid;
    }, { .max_workers = conc });
    return v;
}

// Bit-determinism across chunking/worker count, and repeatability at a fixed concurrency.
void test_colored_determinism()
{
    std::vector<float> serial = relax_chain(64, 5, 1);
    TS_CHECK(serial == relax_chain(64, 5, 2));
    std::vector<float> wide = relax_chain(64, 5, 8);
    TS_CHECK(serial == wide);
    TS_CHECK(wide == relax_chain(64, 5, 8));   // repeatable at the same (high) concurrency
}

// The no-op early-outs and the capping/late-helper paths, from a blue thread (no grant needed -
// the body touches no guarded state).
void test_colored_edges()
{
    std::atomic<int> calls{ 0 };
    auto body = [&calls](int) { calls.fetch_add(1, std::memory_order_relaxed); };

    ts::parallel_for_colored({ { 0, 1 } }, 0, body);        // rounds <= 0
    ts::parallel_for_colored({ {}, {} }, 5, body);          // all bands empty
    ts::parallel_for_colored({}, 5, body);                  // no bands
    TS_CHECK(calls.load() == 0);

    calls.store(0);
    ts::parallel_for_colored({ { 7 } }, 4, body, { .max_workers = 1 });   // single item, serial
    TS_CHECK(calls.load() == 4);

    calls.store(0);
    ts::parallel_for_colored({ { 0, 1 } }, 2, body, { .max_workers = 8 }); // band narrower than conc
    TS_CHECK(calls.load() == 4);   // 2 items x 2 rounds; surplus executors find nothing, no hang

    calls.store(0);
    ts::parallel_for_colored({ { 0 }, {}, { 2 } }, 3, body, { .max_workers = 4 }); // interior empty band filtered
    TS_CHECK(calls.load() == 6);   // 2 non-empty items x 3 rounds
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
    run("parallel_for_async", test_parallel_for_async);
    run("parallel_for nested", test_parallel_for_nested);
    run("parallel_for inherits grant", test_parallel_for_inherits_grant);
    run_if(with_rule_in_task_sync, "TS_RULE_IN_TASK_SYNC off",
           "parallel_for inherits relaxation", test_parallel_for_inherits_relaxation);
    run("parallel_for priorities", test_parallel_for_priorities);
    run("parallel_for priority inheritance", test_parallel_for_priority_inheritance);
    run("parallel_for priority order", test_parallel_for_priority_order);
    run("parallel_for_async cancel", test_parallel_for_async_cancel);
    run("parallel_for current scheduler", test_parallel_for_current_scheduler);
    run("parallel_for_colored counts", test_colored_counts);
    run("parallel_for_colored determinism across concurrency", test_colored_determinism);
    run("parallel_for_colored edges", test_colored_edges);
}
