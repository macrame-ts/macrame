#include "harness.h"
#include "ts/fatal.h"

#include <cstdio>
#include <cstdlib>
#include <string>
// As in src/fatal.cpp: <version> defines the feature-test macro; without it libstdc++
// leaves __cpp_lib_stacktrace undefined here and the guard silently disables traces.
#include <version>

// Same guard as src/fatal.cpp: `std::stacktrace` is C++23 but not in every stdlib the
// Linux TSan build sees, and a failing check's message plus the sanitizer's own backtrace
// is enough there.
#if defined(__cpp_lib_stacktrace) && __has_include(<stacktrace>)
    #include <stacktrace>
    #define TS_TEST_HAVE_STACKTRACE 1
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

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
        message ? "  - " : "",
        message ? message : "");

#if TS_TEST_HAVE_STACKTRACE
    std::string trace = std::to_string(std::stacktrace::current());
    std::fprintf(stderr, "%s\n", trace.c_str());
#endif
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

// Runs one `--death` scenario in a child process and reports whether it died. "Died" is
// any outcome other than a clean zero exit: on Windows `abort()` surfaces as a non-zero
// exit code, on POSIX it surfaces as termination by SIGABRT, which `waitpid` reports as a
// signal rather than an exit status - both count, so the two platforms agree on what a
// death test means.
bool expect_death(const char* scenario)
{
#if defined(_WIN32)
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    intptr_t rc = _spawnl(_P_WAIT, path, path, "--death", scenario, static_cast<const char*>(nullptr));
    return rc != 0;
#else
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0)
        return false;
    path[n] = '\0';
    pid_t pid = fork();
    if (pid == 0)
    {
        execl(path, path, "--death", scenario, static_cast<const char*>(nullptr));
        _exit(127);   // exec failed: a non-zero exit, read as "died" below
    }
    if (pid < 0)
        return false;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return false;
    if (WIFSIGNALED(status))
        return true;
    return WIFEXITED(status) && WEXITSTATUS(status) != 0;
#endif
}

void prepare_death_child()
{
#if defined(_WIN32)
    // suppress the `abort()` message box and WER dialog so the child exits cleanly
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
#endif
    // POSIX: `abort()` raises SIGABRT with no dialog, so there is nothing to suppress.
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

namespace
{
// Ensure failures fired before this run started; `summary()` counts only this run's.
long long g_ensure_baseline = 0;
}

void reset()
{
    g_checks = 0;
    g_failures = 0;
    g_current_test_failures = 0;
    g_skipped = 0;
    g_ensure_failures_consumed = 0;
#if TS_SAFETY_CHECKS
    g_ensure_baseline = ts::ensure_failure_count();
#endif
}

int summary()
{
#if TS_SAFETY_CHECKS
    // Unconsumed ensure failures fail the run: a test that trips `TS_ENSURE` without
    // declaring it (`consume_ensure_failures`) is a regression, even when every
    // TS_CHECK passed.
    long long unconsumed = (ts::ensure_failure_count() - g_ensure_baseline) - g_ensure_failures_consumed;
    if (unconsumed != 0)
    {
        std::fprintf(stderr,
            "\n%lld unconsumed ENSURE failure(s) (fired %lld, consumed %lld)\n",
            unconsumed, ts::ensure_failure_count() - g_ensure_baseline, g_ensure_failures_consumed);
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
