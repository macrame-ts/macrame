#include "harness.h"

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

bool expect_death(const char* scenario)
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);

    // _P_WAIT returns the child's exit code; abort() yields a non-zero/abnormal code.
    intptr_t rc = _spawnl(_P_WAIT, path, path, "--death", scenario, static_cast<const char*>(nullptr));
    return rc != 0;
}

void prepare_death_child()
{
    // suppress the abort() message box and WER dialog so the child exits cleanly
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
}

int summary()
{
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

} // namespace ts::test
