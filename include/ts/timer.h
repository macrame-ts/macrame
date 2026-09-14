#pragma once

// Time-driven waits. `ts::sleep` / `ts::sleep_until` return a task that settles at a deadline,
// and `ts::Periodic` is a fixed-rate tick source built on them - the clock a fixed-rate graph
// runs on (docs/guide.md, "Fixed-rate subsystems"). Design of record:
// docs/internals/timer-primitive-design.md.

#include "ts/cancellation.h"
#include "ts/priority.h"
#include "ts/task.h"

#include <chrono>
#include <optional>
#include <source_location>

namespace ts
{

// Options for the time-driven waits.
struct Sleep_options
{
    // Cancels the wait: the returned task settles cancelled promptly instead of at the deadline.
    Cancellation_token token = {};
    // Priority of the task that delivers the wakeup, which is where an awaiting coroutine
    // resumes. Unset = the calling task's priority, else `normal` (`detail::resolved_priority`).
    std::optional<Priority> priority{};
};

// A task that settles completed at `deadline`, or earlier and cancelled once `opts.token` is
// requested. Deadlines are kept by one timer thread, created on first use and stopped by
// `destroy_scheduler`. It runs no user code: the wakeup is delivered as a task at
// `opts.priority`, so a coroutine awaiting the sleep resumes on a worker. A deadline already
// passed returns a settled task. Worker-less mode has no worker to deliver on and is fatal.
// Await or cancel every sleep before `destroy_scheduler` (fatal under `TS_SAFETY_CHECKS`).
[[nodiscard("the returned task is the wait: co_await or sync it")]]
Task<void> sleep_until(std::chrono::steady_clock::time_point deadline, Sleep_options opts = {},
                       std::source_location site = std::source_location::current());

// `sleep_until(now + duration, opts)`.
[[nodiscard("the returned task is the wait: co_await or sync it")]]
Task<void> sleep(std::chrono::steady_clock::duration duration, Sleep_options opts = {},
                 std::source_location site = std::source_location::current());

// A fixed-rate tick source. Deadlines sit on the grid `origin + k * period` (origin = the
// construction instant), so late delivery never drifts the grid:
//
//   ts::Periodic tick{ 16'667us, { .priority = ts::Priority::high } };
//   for (;;)
//   {
//       int due = co_await tick.next();   // grid points passed since the previous tick
//       ...
//   }
//
// `next()` settles with the number of grid points passed since the previous `next()` settled:
// 1 in steady state, more when the consumer fell behind (what to do about it is the
// consumer's policy), and 0 once `opts.token` is requested. One consumer awaits `next()` at a
// time, and the `Periodic` must outlive the task `next()` returns.
class Periodic
{
public:
    explicit Periodic(std::chrono::steady_clock::duration period, Sleep_options opts = {},
                      std::source_location site = std::source_location::current());

    [[nodiscard("the returned task is the tick: co_await or sync it")]]
    Task<int> next();

    // Re-anchor the grid at now: the next deadline is one period away. For resuming after a
    // deliberate pause, which would otherwise report every missed period at once.
    void reset();

    std::chrono::steady_clock::duration period() const noexcept { return period_; }

private:
    // Grid points passed since the previous tick, advancing the grid past now; 0 once the
    // token is requested. The body of the task `next()` returns.
    int advance() noexcept;

    std::chrono::steady_clock::duration period_;
    std::chrono::steady_clock::time_point next_deadline_;
    Sleep_options opts_;
    std::source_location site_;
};

namespace detail
{
// Stop the timer thread and join it (the next sleep restarts it). With `check_armed`, a sleep
// still armed is fatal under `TS_SAFETY_CHECKS` - `destroy_scheduler` passes true, program exit
// false. Armed sleeps dropped here never settle.
void timer_shutdown(bool check_armed) noexcept;
}

} // namespace ts
