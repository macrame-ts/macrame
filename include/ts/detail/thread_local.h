#pragma once

// The barrier every thread-local in the library is reached through.
//
// A coroutine frame holding state across a suspension invites the compiler to resolve a
// thread-local's block address once and reuse it after the resume - which is the address on
// the thread that suspended, not the one that resumed. MSVC (19.51) does this: it spills the
// block pointer into the coroutine frame and reloads it for every later access, so a resumed
// body reads and writes another thread's slot. The shape that reproduces it is a guard
// acquired inside a loop containing a suspension: the loop-invariant address is hoisted above
// the suspension point (the LICM-across-`coro.suspend` case). Out of line, the block is
// resolved where the access happens.
//
// The bug class is cross-compiler and its upstream fix is incomplete: LLVM #47179 (2020,
// "[coroutines] Compiler incorrectly caches thread_local address across suspend-points") was
// refiled as #63022 (2023) because the first fix did not cover every scenario, and D92661
// ("[RFC] Fix TLS and Coroutine") is the standing proposal; there is an MSVC Developer
// Community report of the same shape. clang-cl 22 and clang 21 recompute the address in the
// scenarios the regression tests exercise, which is not immunity. The hazard also is not
// limited to `thread_local`: anything thread-identifying can be cached the same way (clang has
// the sibling case for `pthread_self()`, and LLVM #72006 is the general form of a frame reused
// as scratch across a suspension).
//
// Two of these were live bugs here, each an asymmetry - one side inlined into a coroutine
// writing through a hoisted address, the other resolving a fresh one: a false rule relaxation
// (`relaxed_rules`) and a false "accessed without declared access" from the harness
// (`current_access`), both reproducing about one run in four.
//
// The barrier is therefore structural rather than a convention. Every thread-local a header
// can reach is a private member of a class whose only interface is out-of-line accessors, so
// no caller can name the variable and the property holds for every toucher instead of being
// re-argued at each one. (A thread-local with internal linkage in a translation unit that
// compiles no coroutine is out of reach by construction and states that at its declaration -
// the worker-less trampoline in scheduler.cpp is the one case.) Two constraints make it hold:
//
//   1. accessors pass by value. A `T&` accessor hands the caching hazard straight back.
//   2. state the compiler cannot pass by value (the trampoline queues) exposes whole
//      operations instead, so the address never leaves the out-of-line frame at all.
//
// This is the shape Rust gives thread-locals by construction (`thread_local!` plus a scoped
// `.with()`, with no way to obtain a raw reference).
//
// Regression tests: "rules relaxed scope reads the resuming thread" (tests/rules_tests.cpp)
// and "thread-local state survives resumption on another worker" (tests/coroutine_tests.cpp).

#include <type_traits>

#if defined(_MSC_VER) && !defined(__clang__)
#define TS_DETAIL_NO_INLINE __declspec(noinline)
#else
#define TS_DETAIL_NO_INLINE [[gnu::noinline]]
#endif

namespace ts::detail
{

// One thread-local scalar, reachable only through out-of-line value accessors. `Derived` is
// the naming type (CRTP), so two variables of the same type are two distinct slots:
//
//   struct Worker_index : Tls_scalar<Worker_index, int, -1> {};
//
// `Init` is a constant initializer, so the slot costs no guard variable and no dynamic
// initialization. Thread-locals whose type is not a scalar cannot use this - they expose
// their operations instead (see `Resume_queue`, `Destroy_queue`).
template<typename Derived, typename T, T Init = T{}>
class Tls_scalar
{
public:
    TS_DETAIL_NO_INLINE static T load() noexcept { return value_; }
    TS_DETAIL_NO_INLINE static void store(T value) noexcept { value_ = value; }

    // Install `value` and hand back what it replaced, for a save/restore scope that would
    // otherwise pay two calls.
    TS_DETAIL_NO_INLINE static T exchange(T value) noexcept
    {
        T prev = value_;
        value_ = value;
        return prev;
    }

    // Read-modify-write in one call, for counters.
    TS_DETAIL_NO_INLINE static T add(T delta) noexcept
        requires std::is_arithmetic_v<T>
    {
        value_ += delta;
        return value_;
    }

private:
    inline static thread_local T value_ = Init;
};

} // namespace ts::detail
