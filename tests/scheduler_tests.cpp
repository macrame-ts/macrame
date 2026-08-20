#include "scheduler_tests.h"
#include "ts/scheduler.h"
#include "harness.h"
#include "test_util.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using ts::test::run;
using tests::wait_until;

namespace
{

void inc(void* p)
{
    static_cast<std::atomic<int>*>(p)->fetch_add(1, std::memory_order_relaxed);
}

// These exercise the scheduler object, but through the single process-wide pool: each test
// reconfigures the global for its scope (`Scheduler_scope`, which tears down + rebuilds the
// one pool) and drives `global_scheduler()`. There are no ad-hoc `Scheduler` instances - at
// most one worker pool ever runs at a time.
void test_single_task()
{
    std::atomic<int> n{ 0 };
    ts::Scheduler_scope scope{ {} };
    ts::Scheduler& s = ts::global_scheduler();
    s.submit(inc, &n);
    wait_until([&] { return n.load() == 1; });
    TS_CHECK(n.load() == 1);
}

void test_many_tasks()
{
    constexpr int count = 5000;
    std::atomic<int> n{ 0 };
    ts::Scheduler_scope scope{ {} };
    ts::Scheduler& s = ts::global_scheduler();
    for (int i = 0; i < count; ++i)
        s.submit(inc, &n);
    wait_until([&] { return n.load() == count; });
    TS_CHECK(n.load() == count);
}

struct Blocker { std::atomic<bool>* started; std::atomic<bool>* go; };
void block_fn(void* p)
{
    auto* b = static_cast<Blocker*>(p);
    b->started->store(true);
    while (!b->go->load()) std::this_thread::yield();
}

struct Rec { std::atomic<int>* idx; int* order; int tag; };
void rec_fn(void* p)
{
    auto* r = static_cast<Rec*>(p);
    // Write the slot before publishing the index, so a reader that observes the
    // new index (with acquire) is guaranteed to see the slot. (Single worker, so
    // the load is unraced.)
    int i = r->idx->load(std::memory_order_relaxed);
    r->order[i] = r->tag;
    r->idx->store(i + 1, std::memory_order_release);
}

// With one worker, a held worker lets us enqueue all three before any runs, so
// they drain in priority order.
void test_priority_order()
{
    ts::Scheduler_scope scope{ { .num_threads = 1 } };
    ts::Scheduler& s = ts::global_scheduler();
    std::atomic<bool> started{ false }, go{ false };
    std::atomic<int> idx{ 0 };
    int order[3] = { -1, -1, -1 };

    Blocker b{ &started, &go };
    s.submit(block_fn, &b);
    wait_until([&] { return started.load(); });   // worker is now inside the blocker

    Rec low{ &idx, order, 2 }, normal{ &idx, order, 1 }, high{ &idx, order, 0 };
    s.submit(rec_fn, &low, ts::Priority::low);
    s.submit(rec_fn, &normal, ts::Priority::normal);
    s.submit(rec_fn, &high, ts::Priority::high);

    go.store(true);
    wait_until([&] { return idx.load() == 3; });
    TS_CHECK(order[0] == 0 && order[1] == 1 && order[2] == 2);
}

void run_mode(ts::Idle_policy policy, int count)
{
    std::atomic<int> n{ 0 };
    ts::Scheduler_scope scope{ { .idle_policy = policy } };
    ts::Scheduler& s = ts::global_scheduler();
    for (int i = 0; i < count; ++i)
        s.submit(inc, &n);
    wait_until([&] { return n.load() == count; });
    TS_CHECK(n.load() == count);
}

void test_spin_mode()            { run_mode(ts::Idle_policy::spin, 2000); }
void test_spin_then_block_mode() { run_mode(ts::Idle_policy::spin_then_block, 2000); }
void test_handoff_mode()         { run_mode(ts::Idle_policy::handoff, 2000); }

// handoff with a single worker: the lone spinner must promote itself across parks correctly
// (a submit while it is parked must wake it via the 0->1 producer wake).
void test_handoff_single_worker()
{
    std::atomic<int> n{ 0 };
    ts::Scheduler_scope scope{ { .num_threads = 1, .idle_policy = ts::Idle_policy::handoff } };
    ts::Scheduler& s = ts::global_scheduler();
    for (int i = 0; i < 2000; ++i)
        s.submit(inc, &n);
    wait_until([&] { return n.load() == 2000; });
    TS_CHECK(n.load() == 2000);
}

// handoff: submit in bursts with idle gaps between, so the pool fully parks and the producer's
// 0->1 wake (not a spinner) is what restarts it each burst.
void test_handoff_bursts()
{
    std::atomic<int> n{ 0 };
    ts::Scheduler_scope scope{ { .idle_policy = ts::Idle_policy::handoff } };
    ts::Scheduler& s = ts::global_scheduler();
    constexpr int bursts = 20, per = 200;
    for (int b = 0; b < bursts; ++b)
    {
        for (int i = 0; i < per; ++i)
            s.submit(inc, &n);
        wait_until([&] { return n.load() == (b + 1) * per; });   // let it fully drain + park
    }
    TS_CHECK(n.load() == bursts * per);
}

void test_shutdown_drains()
{
    constexpr int count = 2000;
    std::atomic<int> n{ 0 };
    {
        ts::Scheduler_scope scope{ {} };
        ts::Scheduler& s = ts::global_scheduler();
        for (int i = 0; i < count; ++i)
            s.submit(inc, &n);
    }   // scope exit reconfigures: tears the pool down (quit + join, draining queued tasks)
    TS_CHECK(n.load() == count);
}

void test_empty_exit()
{
    { ts::Scheduler_scope scope{ { .idle_policy = ts::Idle_policy::spin } }; }
    { ts::Scheduler_scope scope{ { .idle_policy = ts::Idle_policy::spin_then_block } }; }
    { ts::Scheduler_scope scope{ { .idle_policy = ts::Idle_policy::handoff } }; }
    TS_CHECK(true);   // reaching here means none hung on shutdown
}

struct Nested { ts::Scheduler* s; std::atomic<int>* n; };
void outer_fn(void* p)
{
    auto* x = static_cast<Nested*>(p);
    x->n->fetch_add(1);
    x->s->submit(inc, x->n);   // submit from within a running task
}

void test_submit_from_task()
{
    ts::Scheduler_scope scope{ {} };
    ts::Scheduler& s = ts::global_scheduler();
    std::atomic<int> n{ 0 };
    Nested x{ &s, &n };
    s.submit(outer_fn, &x);
    wait_until([&] { return n.load() == 2; });
    TS_CHECK(n.load() == 2);
}

void test_stress()
{
    constexpr int count = 100000;
    std::atomic<int> n{ 0 };
    {
        ts::Scheduler_scope scope{ {} };
        ts::Scheduler& s = ts::global_scheduler();
        for (int i = 0; i < count; ++i)
            s.submit(inc, &n);
    }
    TS_CHECK(n.load() == count);
}

struct Order_rec { std::atomic<int>* seq; std::atomic<int>* out; };
void order_fn(void* p)
{
    auto* r = static_cast<Order_rec*>(p);
    int o = r->seq->fetch_add(1);
    if (r->out)
        r->out->store(o);
}

// The starvation valve: under a steady stream of `normal` work, a `low` task still makes
// progress (served every N high/normal) instead of waiting for normal to drain. One worker,
// held while we queue 1 low + many normal, so the ordering is deterministic. Without the
// valve `low` runs last (order == normals); with it, it runs mid-stream (well before the end).
void test_low_starvation_valve()
{
    ts::Scheduler_scope scope{ { .num_threads = 1 } };
    ts::Scheduler& s = ts::global_scheduler();
    std::atomic<bool> started{ false }, go{ false };
    std::atomic<int> seq{ 0 }, low_order{ -1 };

    Blocker b{ &started, &go };
    s.submit(block_fn, &b);
    wait_until([&] { return started.load(); });   // worker is held inside the blocker

    Order_rec normal_rec{ &seq, nullptr };
    Order_rec low_rec{ &seq, &low_order };
    s.submit(order_fn, &low_rec, ts::Priority::low);
    constexpr int normals = 200;
    for (int i = 0; i < normals; ++i)
        s.submit(order_fn, &normal_rec, ts::Priority::normal);

    go.store(true);
    wait_until([&] { return seq.load() == normals + 1; });

    TS_CHECK(low_order.load() >= 0);          // low ran
    TS_CHECK(low_order.load() < normals);     // and not last - the valve served it mid-stream
}

// Worker-less mode: no workers exist, and a submitted task executes inline, on the
// submitting thread, before `submit` returns.
void test_single_threaded_inline()
{
    ts::Scheduler_scope scope{ { .single_threaded = true } };
    ts::Scheduler& s = ts::global_scheduler();
    TS_CHECK(s.worker_count() == 0);

    struct Probe
    {
        std::thread::id ran_on{};
        bool ran = false;
    } probe;
    s.submit(+[](void* p)
    {
        auto* pr = static_cast<Probe*>(p);
        pr->ran_on = std::this_thread::get_id();
        pr->ran = true;
    }, &probe);

    TS_CHECK(probe.ran);   // completed before submit returned - no wait needed
    TS_CHECK(probe.ran_on == std::this_thread::get_id());
}

// Chained submissions from inside a running body drain FIFO via the trampoline: the
// running body finishes before its submissions run, and they run in submission order.
void test_single_threaded_chain_order()
{
    ts::Scheduler_scope scope{ { .single_threaded = true } };
    ts::Scheduler& s = ts::global_scheduler();

    struct Chain
    {
        ts::Scheduler* s;
        std::vector<int> order;
    } chain{ &s, {} };

    s.submit(+[](void* p)
    {
        auto* c = static_cast<Chain*>(p);
        c->s->submit(+[](void* q) { static_cast<Chain*>(q)->order.push_back(2); }, c);
        c->s->submit(+[](void* q) { static_cast<Chain*>(q)->order.push_back(3); }, c);
        c->order.push_back(1);   // the body completes before its submissions run
    }, &chain);

    TS_CHECK((chain.order == std::vector<int>{ 1, 2, 3 }));
}

} // namespace

void run_scheduler_tests()
{
    std::printf("\n[scheduler] tests\n");
    run("single task", test_single_task);
    run("many tasks", test_many_tasks);
    run("priority order (1 worker)", test_priority_order);
    run("spin mode", test_spin_mode);
    run("spin_then_block mode", test_spin_then_block_mode);
    run("handoff mode", test_handoff_mode);
    run("handoff single worker", test_handoff_single_worker);
    run("handoff bursts", test_handoff_bursts);
    run("shutdown drains", test_shutdown_drains);
    run("empty queue exits", test_empty_exit);
    run("submit from task", test_submit_from_task);
    run("low starvation valve", test_low_starvation_valve);
    run("single-threaded: inline at submit", test_single_threaded_inline);
    run("single-threaded: FIFO chain order", test_single_threaded_chain_order);
    run("stress 100k", test_stress);
    run("death: second create_scheduler is fatal (singleton)",
        []{ TS_CHECK(ts::test::expect_death("scheduler_double_create")); });
    run("death: global_scheduler with none running",
        []{ TS_CHECK(ts::test::expect_death("scheduler_use_after_destroy")); });
    run("death: destroy_scheduler with none running",
        []{ TS_CHECK(ts::test::expect_death("scheduler_destroy_twice")); });
}
