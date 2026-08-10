#pragma once

#include "ts/fatal.h"
#include "ts/rules.h"   // the rule mask, for the `with_rule_*` predicates below

// `TS_PROFILING` is owned by scheduler.h/static_task_graph.h; repeated idempotently, as
// elsewhere, so the harness need not include either.
#ifndef TS_PROFILING
#define TS_PROFILING 1
#endif

namespace ts::test
{

// Configuration predicates for `run_if` (below). Named rather than spelled inline at each
// call site so "which switch does this test need" is one token, and adding a switch does
// not mean auditing `#if`s scattered through the suite.
//
// A death test must name the switch that compiles in the fatal it expects, not merely
// `with_harness`: when the fatal is absent the child does not abort, and for most of these
// it does the thing the fatal was preventing - which is usually a deadlock, so the parent
// hangs in `_spawnl(_P_WAIT)` rather than failing. That is how the Shipping suite came to
// hang after its crash was fixed.
inline constexpr bool with_harness = TS_SAFETY_CHECKS != 0;      // TS_CHECK_ACCESS + the harness fatals
inline constexpr bool with_profiling = TS_PROFILING != 0;        // the DOT dump and the trace
inline constexpr bool with_rule_in_task_sync = TS_RULE_ON(TS_RULE_IN_TASK_SYNC);
inline constexpr bool with_rule_await_under_guard = TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD);
inline constexpr bool with_rule_access_rank = TS_RULE_ON(TS_RULE_ACCESS_RANK);
inline constexpr bool with_rule_circular_wait = TS_RULE_ON(TS_RULE_CIRCULAR_WAIT);
inline constexpr bool with_rule_deadlock_net = TS_RULE_ON(TS_RULE_DEADLOCK_NET);

// Records a check. On failure: prints location + the call stack and marks the
// current test failed, but does not abort (tests are the one place failures are
// non-fatal). Returns the condition.
bool record_check(bool passed, const char* expr, const char* file, int line, const char* message);

// Run a named test function; per-test pass/fail is printed from recorded checks.
using Test_fn = void(*)();
void run(const char* name, Test_fn fn);

// Run `fn` only when the facility it exercises exists in this build; otherwise record it as
// skipped, with `reason` naming the switch. `summary()` reports the count, so a test that
// does not apply to a configuration is visible rather than silently absent - which is how
// the Shipping configuration rotted into six failures and a crash without anyone noticing.
//
// This is the one sanctioned way to make a test configuration-conditional. Do not wrap the
// registration in `#if`: a test that vanishes leaves no trace in the output, and the next
// compiled-out check repeats the mistake.
//
//   run_if(with_harness, "TS_SAFETY_CHECKS=0", "death: no context", test_death_no_context);
void run_if(bool available, const char* reason, const char* name, Test_fn fn);

// Spawn this executable with "--death <scenario>" and return true if it aborted.
// Used to test fatal paths without exceptions.
bool expect_death(const char* scenario);

// Configure the current process as a death-test child (suppress abort dialogs).
void prepare_death_child();

// Declare that the current test deliberately triggered `n` `TS_ENSURE` failures.
// `summary()` fails on any failure not consumed this way, so a stray ensure failure
// in an unrelated test cannot pass silently. No-op when the facility is compiled out.
void consume_ensure_failures(int n);

#if TS_SAFETY_CHECKS
// RAII guard for a test that deliberately trips `TS_ENSURE`. Installs a silent
// presentation handler for its lifetime, so the default debugger-break does not stop
// a deliberately-failing test under an attached debugger (F5); on destruction it
// consumes `expected` failures (keeping `summary()` green) and restores the handler
// that was installed before. The failure counter still advances (counting lives in
// `ensure_failed`, outside the handler), so a test can assert the exact count while
// the guard is alive. Non-copyable, non-movable.
class Expected_ensures
{
public:
    explicit Expected_ensures(int expected) noexcept;
    ~Expected_ensures();

    Expected_ensures(const Expected_ensures&) = delete;
    Expected_ensures& operator=(const Expected_ensures&) = delete;

private:
    int expected_;
    ts::Ensure_handler prev_;
};
#endif

// Print totals; returns a process exit code (0 = all checks passed and no
// unconsumed ensure failures).
int summary();

} // namespace ts::test

#define TS_CHECK(cond) \
    ::ts::test::record_check((cond), #cond, __FILE__, __LINE__, nullptr)

#define TS_CHECK_MSG(cond, msg) \
    ::ts::test::record_check((cond), #cond, __FILE__, __LINE__, (msg))
