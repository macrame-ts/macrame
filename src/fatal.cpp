#include "ts/fatal.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

// std::stacktrace is C++23 but not yet in every stdlib (e.g. some libstdc++/libc++
// configs used for the Linux TSan build). Use it when available; otherwise the
// message + the sanitizer's own backtrace suffices.
#if defined(__cpp_lib_stacktrace) && __has_include(<stacktrace>)
    #include <stacktrace>
    #include <string>
    #define TS_HAVE_STACKTRACE 1
#endif

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    #include <dbghelp.h>
    #pragma comment(lib, "dbghelp.lib")
#elif defined(__APPLE__)
    #include <sys/sysctl.h>
    #include <unistd.h>
#endif

namespace ts
{

void fatal(const char* message) noexcept
{
    std::fprintf(stderr, "\nFATAL: %s\n", message);

#if TS_HAVE_STACKTRACE
    std::fprintf(stderr, "\ncall stack:\n%s\n",
        std::to_string(std::stacktrace::current()).c_str());
#endif

    std::fflush(stderr);
#if TS_SAFETY_CHECKS
    // Stop in the debugger at the failure site (message already on screen)
    // instead of somewhere inside `abort`'s runtime machinery.
    if (detail::is_debugger_present())
        detail::debug_break();
#endif
#if defined(_WIN32)
    // Die fast rather than modally. The message and the stack are already on stderr, so the
    // CRT `abort()` box and the WER dialog add no diagnostic value - and on a headless host
    // (CI, a server, a spawned death-test child) nothing dismisses them, which turns a crash
    // into an indefinite hang. Scoped to the imminent abort; the AV/stack-overflow minidump
    // path runs through `install_crash_handler`'s unhandled-exception filter and is untouched.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::abort();
}

#if TS_SAFETY_CHECKS

namespace detail
{

#if defined(_WIN32)

bool is_debugger_present() noexcept
{
    return ::IsDebuggerPresent() != 0;
}

void debug_break() noexcept
{
    __debugbreak();
}

#elif defined(__linux__)

bool is_debugger_present() noexcept
{
    // A tracer (debugger) registers as `TracerPid` in the process status; 0 = none.
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f)
        return false;
    bool traced = false;
    char line[256];
    while (std::fgets(line, sizeof line, f))
    {
        int pid = 0;
        if (std::sscanf(line, "TracerPid: %d", &pid) == 1)
        {
            traced = pid != 0;
            break;
        }
    }
    std::fclose(f);
    return traced;
}

void debug_break() noexcept
{
#if defined(__clang__)
    __builtin_debugtrap();
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("int3");
#elif defined(__aarch64__)
    __asm__ volatile("brk #0");
#endif
}

#elif defined(__APPLE__)

bool is_debugger_present() noexcept
{
    // Best-effort: `P_TRACED` in the process flags via sysctl.
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    struct kinfo_proc info{};
    size_t size = sizeof info;
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0)
        return false;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
}

void debug_break() noexcept
{
    __builtin_debugtrap();
}

#else

bool is_debugger_present() noexcept { return false; }
void debug_break() noexcept {}

#endif

} // namespace detail

namespace
{

std::atomic<long long> g_ensure_failure_count{ 0 };

// Default presentation: message + call stack to stderr, then a debugger break
// (after the report, so the message is on screen when the debugger stops).
void default_ensure_handler(const char* message) noexcept
{
    std::fprintf(stderr, "\nENSURE FAILED: %s\n", message);

#if TS_HAVE_STACKTRACE
    std::fprintf(stderr, "\ncall stack:\n%s\n",
        std::to_string(std::stacktrace::current()).c_str());
#endif

    std::fflush(stderr);

    if (detail::is_debugger_present())
        detail::debug_break();
}

std::atomic<Ensure_handler> g_ensure_handler{ &default_ensure_handler };

}

Ensure_handler set_ensure_handler(Ensure_handler handler) noexcept
{
    return g_ensure_handler.exchange(handler ? handler : &default_ensure_handler,
                                     std::memory_order_acq_rel);
}

long long ensure_failure_count() noexcept
{
    return g_ensure_failure_count.load(std::memory_order_relaxed);
}

namespace detail
{

void ensure_failed(const char* message, bool report) noexcept
{
    // Counting and once-per-site filtering happen out here regardless of the
    // installed handler - a host handler only changes presentation, never
    // whether the harness/drivers see the failure.
    g_ensure_failure_count.fetch_add(1, std::memory_order_relaxed);
    if (report)
        g_ensure_handler.load(std::memory_order_acquire)(message);
}

} // namespace detail

#else // TS_SAFETY_CHECKS

// The hook stays declared with the checks off so a host that installs a handler
// compiles unchanged; with no ensure able to fire, both verbs are inert.
Ensure_handler set_ensure_handler(Ensure_handler) noexcept
{
    return nullptr;
}

long long ensure_failure_count() noexcept
{
    return 0;
}

#endif // TS_SAFETY_CHECKS

#if defined(_WIN32)

namespace
{

// Last-resort handler for a structured exception that no `fatal` produced - a
// raw access violation being the case that motivated this (it never routes
// through `fatal`, so without this the process dies with no message and no
// stack). Runs on the faulting thread with its stack intact, so both the printed
// `std::stacktrace` and the minidump capture the real fault site.
LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) noexcept
{
    const unsigned long code = (info && info->ExceptionRecord)
        ? info->ExceptionRecord->ExceptionCode : 0;
    const void* addr = (info && info->ExceptionRecord)
        ? info->ExceptionRecord->ExceptionAddress : nullptr;
    std::fprintf(stderr, "\nFATAL (unhandled exception): code 0x%08lX at %p\n", code, addr);

#if TS_HAVE_STACKTRACE
    // Symbols resolve only if a PDB sits next to the exe (the Release CI build now
    // ships one). The top frames are this handler; the fault site is just below.
    std::fprintf(stderr, "\ncall stack (crash handler frames on top):\n%s\n",
        std::to_string(std::stacktrace::current()).c_str());
#endif

    const char* dump_path = "macrame_crash.dmp";
    HANDLE file = CreateFileA(dump_path, GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithThreadInfo | MiniDumpWithHandleData | MiniDumpWithDataSegs);
        const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
            file, type, info ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(file);
        std::fprintf(stderr, ok ? "minidump written: %s\n" : "minidump write FAILED: %s\n",
            dump_path);
    }

    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;   // terminate; do not hand off to WER (avoids a hang)
}

} // namespace

void install_crash_handler() noexcept
{
    SetUnhandledExceptionFilter(&crash_filter);
}

#else

void install_crash_handler() noexcept {}

#endif

} // namespace ts
