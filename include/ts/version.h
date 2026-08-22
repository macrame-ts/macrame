#pragma once

// Library version - the single source of truth for the version number.
// Keep in sync with CMakeLists.txt project(version ...).

#define TS_VERSION_MAJOR 0
#define TS_VERSION_MINOR 1
#define TS_VERSION_PATCH 0
#define TS_VERSION_STRING "0.1.0"

namespace ts
{

// The same numbers as the macros above, usable where a macro is not: a template
// argument, a constant expression in a namespace-scoped constant, a value passed
// to a logging call. The macros stay for preprocessor conditionals.
inline constexpr int version_major = TS_VERSION_MAJOR;
inline constexpr int version_minor = TS_VERSION_MINOR;
inline constexpr int version_patch = TS_VERSION_PATCH;

// "major.minor.patch"; a static string, valid for the process lifetime.
constexpr const char* version_string() noexcept
{
    return TS_VERSION_STRING;
}

} // namespace ts
