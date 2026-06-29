#include "scheduler_tests.h"
#include "scheduler.h"
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
    Scheduler s;
    s.submit(inc, &n);
    wait_until([&] { return n.load() == 1; });
    TS_CHECK(n.load() == 1);
}

void test_many_tasks()
{
    constexpr int count = 5000;
    std::atomic<int> n{ 0 };
    Scheduler s;
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
    int i = r->idx->fetch_add(1);
    r->order[i] = r->tag;
}

// With one worker, a held worker lets us enqueue all three before any runs, so
// they drain in priority order.
void test_priority_order()
{
    Scheduler s{ { .num_threads = 1 } };
    std::atomic<bool> started{ false }, go{ false };
    std::atomic<int> idx{ 0 };
    int order[3] = { -1, -1, -1 };

    Blocker b{ &started, &go };
    s.submit(block_fn, &b);
    wait_until([&] { return started.load(); });   // worker is now inside the blocker

    Rec low{ &idx, order, 2 }, normal{ &idx, order, 1 }, high{ &idx, order, 0 };
    s.submit(rec_fn, &low, Priority::low);
    s.submit(rec_fn, &normal, Priority::normal);
    s.submit(rec_fn, &high, Priority::high);

    go.store(true);
    wait_until([&] { return idx.load() == 3; });
    TS_CHECK(order[0] == 0 && order[1] == 1 && order[2] == 2);
}

void run_mode(Idle_policy policy, int count)
{
    std::atomic<int> n{ 0 };
    Scheduler s{ { .idle_policy = policy } };
    for (int i = 0; i < count; ++i)
        s.submit(inc, &n);
    wait_until([&] { return n.load() == count; });
    TS_CHECK(n.load() == count);
}

void test_block_mode() { run_mode(Idle_policy::block, 2000); }
void test_spin_mode()  { run_mode(Idle_policy::spin, 2000); }

void test_shutdown_drains()
{
    constexpr int count = 2000;
    std::atomic<int> n{ 0 };
    {
        Scheduler s;
        for (int i = 0; i < count; ++i)
            s.submit(inc, &n);
    }   // destructor sets quit + joins; queued tasks must have run
    TS_CHECK(n.load() == count);
}

void test_empty_exit()
{
    { Scheduler s{ { .idle_policy = Idle_policy::block } }; }
    { Scheduler s{ { .idle_policy = Idle_policy::spin } }; }
    TS_CHECK(true);   // reaching here means neither hung on shutdown
}

struct Nested { Scheduler* s; std::atomic<int>* n; };
void outer_fn(void* p)
{
    auto* x = static_cast<Nested*>(p);
    x->n->fetch_add(1);
    x->s->submit(inc, x->n);   // submit from within a running task
}

void test_submit_from_task()
{
    Scheduler s;
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
        Scheduler s;
        for (int i = 0; i < count; ++i)
            s.submit(inc, &n);
    }
    TS_CHECK(n.load() == count);
}

} // namespace

void run_scheduler_tests()
{
    std::printf("\n[scheduler] tests\n");
    run("single task", test_single_task);
    run("many tasks", test_many_tasks);
    run("priority order (1 worker)", test_priority_order);
    run("block mode", test_block_mode);
    run("spin mode", test_spin_mode);
    run("shutdown drains", test_shutdown_drains);
    run("empty queue exits", test_empty_exit);
    run("submit from task", test_submit_from_task);
    run("stress 100k", test_stress);
}
