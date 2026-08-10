#pragma once

#include <atomic>

#ifndef TS_SAFETY_CHECKS
#define TS_SAFETY_CHECKS 1
#endif

namespace ts
{

// Print a message and the current call stack, then abort (breaking into the
// debugger first when one is attached, so the stop lands at the failure site
// rather than inside `abort`). Used for all non-recoverable failures (the
// project is built with exceptions disabled).
[[noreturn]] void fatal(const char* message) noexcept;

#if TS_SAFETY_CHECKS

// `TS_ENSURE(expr, message)` - the recoverable assert, UE-`ensure`-shaped.
// `expr` is evaluated exactly once in every configuration and the macro yields
// its boolean value, so `if (!TS_ENSURE(x, "..."))` recovers naturally. On a
// false result (checked builds only): the failure counter bumps on every
// occurrence, but the installed handler reports once per call site - a
// violation recurring every frame stays one stack trace in the log while the
// counter (and the harness) still see the exact count. The default handler
// prints `ensure failed: <message>` plus the call stack to stderr, then breaks
// into the debugger when one is attached. For hazards that deserve a loud
// diagnostic but are not certain corruption (e.g. a blocking `sync()` inside
// an access scope). With `TS_SAFETY_CHECKS` 0 the macro is just the expression.

// Presentation hook (`std::set_terminate` shape): a host application may
// replace how a failure is reported - e.g. its own attach-a-debugger dialog.
// Returns the previous handler. The failure counter and the once-per-site
// filtering are not the handler's concern; it only presents.
using Ensure_handler = void(*)(const char* message) noexcept;
Ensure_handler set_ensure_handler(Ensure_handler handler) noexcept;

// Total `TS_ENSURE` failures since process start - every occurrence, not just
// the reported-once ones (relaxed; diagnostic). The test harness fails on
// failures no test consumed; the bench/stress drivers fail their exit code on
// any, so a tripped ensure cannot pass CI behind a surviving process.
long long ensure_failure_count() noexcept;

namespace detail
{

// The C++26 `std::is_debugger_present` / `std::breakpoint` pair (P2546),
// polyfilled per platform until the standard library carries it; slots into
// the future platform layer (docs/TODO.md 3.6).
bool is_debugger_present() noexcept;
void debug_break() noexcept;

// `TS_ENSURE`'s slow path: bump the failure counter; invoke the installed
// handler when `report` is set (the macro's per-site once-claim). Direct
// callers wanting per-site semantics pass their own claim (or use
// `TS_ENSURE(false, ...)`).
void ensure_failed(const char* message, bool report) noexcept;

} // namespace detail

#endif // TS_SAFETY_CHECKS

// Install a process-wide last-resort handler for crashes that never reach
// `fatal` - a raw access violation, stack overflow, or any other unhandled
// structured exception. It prints the exception code + a call stack and writes
// a minidump before terminating, so a crash self-reports instead of dying
// silently (the CI runner otherwise shows only a bare non-zero exit). Call once
// at process start. No-op on non-Windows (sanitizers provide their own report).
void install_crash_handler() noexcept;

#if TS_SAFETY_CHECKS
// The captureless lambda is the per-site once-claim: its function-local static
// is unique to the expansion site, so the first failure there reports and the
// rest only count. `expr` contributes the macro's value in both branches of
// the `||`, preserving UE `ensure` semantics (evaluate once, yield the bool).
#define TS_ENSURE(expr, message) \
    ((!!(expr)) || (::ts::detail::ensure_failed((message), \
        []() noexcept -> bool \
        { \
            static std::atomic<bool> reported{ false }; \
            return !reported.exchange(true, std::memory_order_relaxed); \
        }()), false))
#else
#define TS_ENSURE(expr, message) (!!(expr))
#endif

} // namespace ts
