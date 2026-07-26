#pragma once

namespace ts::test
{

// Records a check. On failure: prints location + the call stack and marks the
// current test failed, but does NOT abort (tests are the one place failures are
// non-fatal). Returns the condition.
bool record_check(bool passed, const char* expr, const char* file, int line, const char* message);

// Run a named test function; per-test pass/fail is printed from recorded checks.
using Test_fn = void(*)();
void run(const char* name, Test_fn fn);

// Spawn this executable with "--death <scenario>" and return true if it aborted.
// Used to test fatal paths without exceptions.
bool expect_death(const char* scenario);

// Configure the current process as a death-test child (suppress abort dialogs).
void prepare_death_child();

// Declare that the current test deliberately triggered `n` `TS_ENSURE` failures.
// `summary()` fails on any failure not consumed this way, so a stray ensure failure
// in an unrelated test cannot pass silently. No-op when the facility is compiled out.
void consume_ensure_failures(int n);

// Print totals; returns a process exit code (0 = all checks passed and no
// unconsumed ensure failures).
int summary();

} // namespace ts::test

#define TS_CHECK(cond) \
    ::ts::test::record_check((cond), #cond, __FILE__, __LINE__, nullptr)

#define TS_CHECK_MSG(cond, msg) \
    ::ts::test::record_check((cond), #cond, __FILE__, __LINE__, (msg))
