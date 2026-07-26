#include "ts/fatal.h"

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
    std::abort();
}

#if defined(_WIN32)

namespace
{

// Last-resort handler for a structured exception that no `fatal` produced -- a
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

    const char* dump_path = "task_system_crash.dmp";
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
