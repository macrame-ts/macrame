#include "harness.h"
#include "ts/fatal.h"

#include <cstdio>
#include <cstdlib>
#include <stacktrace>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>

namespace ts::test
{
namespace
{

int g_checks = 0;
int g_failures = 0;
int g_current_test_failures = 0;
int g_skipped = 0;
long long g_ensure_failures_consumed = 0;

#if TS_SAFETY_CHECKS
// Installed by `Expected_ensures` so a deliberately-failing test presents nothing and
// does not break into an attached debugger. Counting stays in `ensure_failed`.
void silent_ensure_handler(const char*) noexcept {}
#endif

} // namespace

bool record_check(bool passed, const char* expr, const char* file, int line, const char* message)
{
    ++g_checks;
    if (passed)
        return true;

    ++g_failures;
    ++g_current_test_failures;

    std::fprintf(stderr, "  FAIL %s:%d  %s%s%s\n",
        file, line, expr,
        message ? "  -- " : "",
        message ? message : "");

    std::string trace = std::to_string(std::stacktrace::current());
    std::fprintf(stderr, "%s\n", trace.c_str());
    return false;
}

void run(const char* name, Test_fn fn)
{
    g_current_test_failures = 0;
    fn();
    std::printf("  [%s] %s\n", g_current_test_failures == 0 ? "ok  " : "FAIL", name);
}

void run_if(bool available, const char* reason, const char* name, Test_fn fn)
{
    if (available)
    {
        run(name, fn);
        return;
    }
    ++g_skipped;
    std::printf("  [skip] %s  (%s)\n", name, reason);
}

bool expect_death(const char* scenario)
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);

    // `_P_WAIT` returns the child's exit code; `abort()` yields a non-zero/abnormal code.
    intptr_t rc = _spawnl(_P_WAIT, path, path, "--death", scenario, static_cast<const char*>(nullptr));
    return rc != 0;
}

void prepare_death_child()
{
    // suppress the `abort()` message box and WER dialog so the child exits cleanly
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
}

void consume_ensure_failures(int n)
{
    g_ensure_failures_consumed += n;
}

#if TS_SAFETY_CHECKS
Expected_ensures::Expected_ensures(int expected) noexcept
    : expected_(expected)
    , prev_(ts::set_ensure_handler(&silent_ensure_handler))
{
}

Expected_ensures::~Expected_ensures()
{
    consume_ensure_failures(expected_);
    ts::set_ensure_handler(prev_);
}
#endif

int summary()
{
#if TS_SAFETY_CHECKS
    // Unconsumed ensure failures fail the run: a test that trips `TS_ENSURE` without
    // declaring it (`consume_ensure_failures`) is a regression, even when every
    // TS_CHECK passed.
    long long unconsumed = ts::ensure_failure_count() - g_ensure_failures_consumed;
    if (unconsumed != 0)
    {
        std::fprintf(stderr,
            "\n%lld unconsumed ENSURE failure(s) (fired %lld, consumed %lld)\n",
            unconsumed, ts::ensure_failure_count(), g_ensure_failures_consumed);
        ++g_failures;
    }
#endif
    if (g_skipped != 0)
    {
        std::printf("\n%d checks, %d failures, %d skipped (not applicable in this configuration)\n",
            g_checks, g_failures, g_skipped);
    }
    else
    {
        std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}

} // namespace ts::test
