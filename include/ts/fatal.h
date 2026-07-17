#pragma once

namespace ts
{

// Print a message and the current call stack, then abort. Used for all
// non-recoverable failures (the project is built with exceptions disabled).
[[noreturn]] void fatal(const char* message) noexcept;

} // namespace ts
