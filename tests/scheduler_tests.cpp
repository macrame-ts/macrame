#include "scheduler_tests.h"
#include "ts/scheduler.h"
#include "harness.h"
#include "test_util.h"

#include <atomic>
#include <cstdio>

using ts::test::run;
using tests::wait_until;

namespace
{

void inc(void* p)
{
    static_cast<std::atomic<int>*>(p)->fetch_add(1, std::memory_order_relaxed);
}

void test_single_task()
{
    std::atomic<int> n{ 0 };
    ts::Scheduler s;
    s.submit(inc, &n);
    wait_until([&] { return n.load() == 1; });
    TS_CHECK(n.load() == 1);
}

void test_many_tasks()
{
    constexpr int count = 5000;
    std::atomic<int> n{ 0 };
    ts::Scheduler s;
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
    // Write the slot BEFORE publishing the index, so a reader that observes the
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
    ts::Scheduler s{ { .num_threads = 1 } };
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
    ts::Scheduler s{ { .idle_policy = policy } };
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
    ts::Scheduler s{ { .num_threads = 1, .idle_policy = ts::Idle_policy::handoff } };
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
    ts::Scheduler s{ { .idle_policy = ts::Idle_policy::handoff } };
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
        ts::Scheduler s;
        for (int i = 0; i < count; ++i)
            s.submit(inc, &n);
    }   // destructor sets quit + joins; queued tasks must have run
    TS_CHECK(n.load() == count);
}

void test_empty_exit()
{
    { ts::Scheduler s{ { .idle_policy = ts::Idle_policy::spin } }; }
    { ts::Scheduler s{ { .idle_policy = ts::Idle_policy::spin_then_block } }; }
    { ts::Scheduler s{ { .idle_policy = ts::Idle_policy::handoff } }; }
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
    ts::Scheduler s;
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
        ts::Scheduler s;
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
// valve `low` runs LAST (order == normals); with it, it runs mid-stream (well before the end).
void test_low_starvation_valve()
{
    ts::Scheduler s{ { .num_threads = 1 } };
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
    TS_CHECK(low_order.load() < normals);     // and NOT last -- the valve served it mid-stream
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
    run("stress 100k", test_stress);
}
