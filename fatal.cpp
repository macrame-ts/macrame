#include "fatal.h"

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

} // namespace ts
