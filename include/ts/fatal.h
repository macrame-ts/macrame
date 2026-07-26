#pragma once

namespace ts
{

// Print a message and the current call stack, then abort. Used for all
// non-recoverable failures (the project is built with exceptions disabled).
[[noreturn]] void fatal(const char* message) noexcept;

// Install a process-wide last-resort handler for crashes that never reach
// `fatal` -- a raw access violation, stack overflow, or any other unhandled
// structured exception. It prints the exception code + a call stack and writes
// a minidump before terminating, so a crash self-reports instead of dying
// silently (the CI runner otherwise shows only a bare non-zero exit). Call once
// at process start. No-op on non-Windows (sanitizers provide their own report).
void install_crash_handler() noexcept;

} // namespace ts
