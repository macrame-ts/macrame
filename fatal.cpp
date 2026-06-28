#include "fatal.h"

#include <cstdio>
#include <cstdlib>
#include <stacktrace>
#include <string>

namespace ts
{

void fatal(const char* message) noexcept
{
    std::fprintf(stderr, "\nFATAL: %s\n\ncall stack:\n", message);

    std::string trace = std::to_string(std::stacktrace::current());
    std::fprintf(stderr, "%s\n", trace.c_str());
    std::fflush(stderr);

    std::abort();
}

} // namespace ts
