#include "timer_tests.h"
#include "harness.h"
#include "ts/coroutine_support.h"
#include "ts/scheduler.h"
#include "ts/task.h"
#include "ts/timer.h"

#include <chrono>
#include <thread>

using ts::test::run;
using namespace ts::test;
using namespace std::chrono_literals;

namespace
{

using Clock = std::chrono::steady_clock;

// A sleep never settles before its deadline.
void test_sleep_waits_for_deadline()
{
    auto t0 = Clock::now();
    ts::sleep(20ms).sync();
    TS_CHECK(Clock::now() - t0 >= 20ms);
}

// A deadline already passed, and a token already requested, both settle inside the call.
void test_sleep_settled_in_call()
{
    ts::Task<void> past = ts::sleep_until(Clock::now() - 1ms);
    TS_CHECK(past.is_done() && !past.is_cancelled());

    ts::Cancellation_source source;
    source.request_cancel();
    ts::Task<void> cancelled = ts::sleep(10s, { .token = source.token() });
    TS_CHECK(cancelled.is_done() && cancelled.is_cancelled());
}

// Cancellation settles the wait promptly and cancelled, not at the deadline.
void test_sleep_cancel_is_prompt()
{
    ts::Cancellation_source source;
    auto t0 = Clock::now();
    ts::Task<void> wait = ts::sleep(10s, { .token = source.token() });
    std::this_thread::sleep_for(5ms);
    source.request_cancel();
    wait.sync();
    TS_CHECK(wait.is_cancelled());
    TS_CHECK(Clock::now() - t0 < 5s);
}

// A deadline earlier than the one the timer thread is parked on wakes it: the earlier sleep
// is not held back until the later deadline.
void test_sleep_earlier_deadline_preempts()
{
    ts::Task<void> late = ts::sleep(400ms);
    ts::Task<void> early = ts::sleep(50ms);
    auto t0 = Clock::now();
    early.sync();
    TS_CHECK(Clock::now() - t0 < 300ms);
    TS_CHECK(!late.is_done());
    late.sync();
}

ts::Task<int> worker_after_sleep()
{
    co_await ts::sleep(5ms);
    co_return ts::current_worker_index();
}

// The wakeup is delivered as a task, so an awaiting coroutine resumes on a worker, never on the
// timer thread.
void test_sleep_resumes_on_worker()
{
    TS_CHECK(worker_after_sleep().sync() >= 0);
}

// Steady state: each `next()` reports at least one grid point, and ten of them take ten periods.
void test_periodic_steady()
{
    ts::Periodic tick{ 5ms };
    auto t0 = Clock::now();
    int total = 0;
    for (int i = 0; i < 10; ++i)
    {
        int due = tick.next().sync();
        TS_CHECK(due >= 1);
        total += due;
    }
    TS_CHECK(total >= 10);
    TS_CHECK(Clock::now() - t0 >= 45ms);
}

// A consumer that falls behind learns how many grid points it missed, in one call.
void test_periodic_reports_missed_ticks()
{
    ts::Periodic tick{ 10ms };
    (void)tick.next().sync();
    std::this_thread::sleep_for(45ms);
    int due = tick.next().sync();
    TS_CHECK(due >= 4);
}

// A requested token ends the tick stream: the pending `next()` settles with 0, promptly.
void test_periodic_cancel()
{
    ts::Cancellation_source source;
    ts::Periodic tick{ 10s, { .token = source.token() } };
    ts::Task<int> pending = tick.next();
    auto t0 = Clock::now();
    source.request_cancel();
    TS_CHECK(pending.sync() == 0);
    TS_CHECK(Clock::now() - t0 < 5s);
}

// A blue thread waiting on a sleep while every worker is idle is not a deadlock: the armed
// sleep registers its pending wakeup with the deadlock net (`External_wait`).
void test_sleep_is_an_external_wait()
{
    ts::set_deadlock_net_window(50ms);
    ts::sleep(300ms).sync();
    ts::set_deadlock_net_window(2000ms);
    TS_CHECK(true);
}

void test_sleep_worker_less_is_fatal()
{
    TS_CHECK(ts::test::expect_death("timer_worker_less"));
}

void test_destroy_with_armed_sleep_is_fatal()
{
    TS_CHECK(ts::test::expect_death("timer_destroy_armed"));
}

} // namespace

void run_timer_tests()
{
    run("timer: sleep waits for its deadline", test_sleep_waits_for_deadline);
    run("timer: past deadline and requested token settle in the call", test_sleep_settled_in_call);
    run("timer: cancellation is prompt", test_sleep_cancel_is_prompt);
    run("timer: an earlier deadline wakes the timer thread", test_sleep_earlier_deadline_preempts);
    run("timer: the awaiting coroutine resumes on a worker", test_sleep_resumes_on_worker);
    run("timer: Periodic steady state", test_periodic_steady);
    run("timer: Periodic reports missed grid points", test_periodic_reports_missed_ticks);
    run("timer: Periodic ends on cancellation", test_periodic_cancel);
    run_if(with_rule_deadlock_net, "TS_ENABLED_RULES without deadlock_net", "timer: an armed sleep is an external wait",
        test_sleep_is_an_external_wait);
    run("timer: sleep in worker-less mode is fatal", test_sleep_worker_less_is_fatal);
    run_if(with_harness, "TS_SAFETY_CHECKS=0", "timer: destroy_scheduler with an armed sleep is fatal",
        test_destroy_with_armed_sleep_is_fatal);
}
