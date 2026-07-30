#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <typeinfo>

#ifndef TS_SAFETY_CHECKS
#define TS_SAFETY_CHECKS 1
#endif

// One `TS_SAFETY_CHECKS` value per binary: the macro changes inline-function bodies and
// class layouts (safety-only fields are fully gated, per the convention in CLAUDE.md), so
// mixing translation units compiled with different values is an ODR violation. Make the
// mistake a LINK error instead of silent corruption: MSVC-family compilers record the
// value per object file and the linker rejects a mismatch outright; elsewhere every
// including TU references a symbol whose name encodes the value and only the matching one
// is defined (src/access.cpp), so a mixed link fails with an unresolved
// `ts::detail::config_safety_checks_*` naming the problem (best-effort: a section-GC'ing
// linker may strip the unreferenced anchor).
#if defined(_MSC_VER)
#define TS_DETAIL_STRINGIZE2(x) #x
#define TS_DETAIL_STRINGIZE(x) TS_DETAIL_STRINGIZE2(x)
#pragma detect_mismatch("TS_SAFETY_CHECKS", TS_DETAIL_STRINGIZE(TS_SAFETY_CHECKS))
#endif

namespace ts
{

namespace detail
{

#if TS_SAFETY_CHECKS
extern const char config_safety_checks_on;
inline const char* const config_tripwire = &config_safety_checks_on;
#else
extern const char config_safety_checks_off;
inline const char* const config_tripwire = &config_safety_checks_off;
#endif

} // namespace detail

enum class Access { read_only, read_write };

// Per-task permission set, installed thread-locally while a task runs. Small by
// design: a task touches a handful of instances, so an inline array + linear
// scan beats a hash set. Identity of an instance is its address.
//
// An entry may carry a GRANT-VALIDITY source: a pointer to the object's pipe
// `write_epoch` plus the value captured when the grant was declared. The epoch has
// seqlock-style parity -- bumped at every write-grant acquire and release (and by +2 on a
// graph write handoff) -- so "epoch unchanged" means the grant window this entry was
// declared under is still the pipe's current one: a write holder's window is still open,
// or no writer has acquired since a read grant was captured. A snapshot copy of the
// context (`snapshot_access`, grant inheritance) carries the captured values with it, so
// a task that outlives the access scope it inherited from fails the comparison and faults
// with a stale-grant diagnostic instead of silently racing the next acquirer. Entries
// without a source (null epoch) never go stale -- hand-built contexts and grant-free
// internal scopes. The epoch pointer is dereferenced only under `TS_SAFETY_CHECKS`; the
// existing lifetime contract (a `Guarded` outlives its accessors) covers its validity.
class Access_context
{
public:
    enum class Grant { none, granted, stale };

    void add(const void* instance, Access mode) noexcept { add(instance, mode, nullptr); }
    void add(const void* instance, Access mode, const std::atomic<std::uint64_t>* epoch) noexcept;
    Grant check(const void* instance, Access mode) const noexcept;
    bool grants(const void* instance, Access mode) const noexcept
    {
        return check(instance, mode) == Grant::granted;
    }

#if TS_SAFETY_CHECKS
    // Whether any entry's grant was declared under the given epoch source (i.e. this
    // context holds a grant on that pipe). Consumed by the blocking-sync diagnostic,
    // which compares a sync target's pipe against the caller's held pipes; entries
    // without a source never match.
    bool holds_epoch(const std::atomic<std::uint64_t>* epoch) const noexcept
    {
        for (int i = 0; i < count_; ++i)
        {
            if (entries_[i].epoch == epoch)
                return true;
        }
        return false;
    }
#endif

private:
    static constexpr int max_entries = 8;

    struct Entry
    {
        const void* instance;
        Access mode;
#if TS_SAFETY_CHECKS
        const std::atomic<std::uint64_t>* epoch;   // null = never stale
        std::uint64_t captured;
#endif
    };

    Entry entries_[max_entries];
    int count_ = 0;
};

namespace detail
{

// null when this thread is not executing a task -> any guarded access faults.
extern thread_local const Access_context* current_access;

[[noreturn]] void access_violation(const char* type_name, Access mode, bool stale = false) noexcept;

// Snapshot the calling thread's access grant by value (empty if no task is running).
// Used to propagate a task's grants to sub-work launched from it (`ts::launch` /
// `ts::nested`): the copy has independent lifetime, so the launched task may run --
// possibly on another thread, possibly after the launcher's body unwinds -- still
// holding the launcher's grant.
inline std::optional<Access_context> snapshot_access()
{
    if (current_access)
        return *current_access;
    return std::nullopt;
}

} // namespace detail

// The harness. Overloaded on this-const-ness, so read/write is deduced from the
// calling method's own const-ness:
//   non-const method -> this is `T*`       -> needs `read_write`
//   const     method -> this is `const T*` -> needs `read_only`
template<typename T>
inline void access_check(T* self) noexcept
{
    const Access_context* ctx = detail::current_access;
    Access_context::Grant g = ctx ? ctx->check(self, Access::read_write) : Access_context::Grant::none;
    if (g != Access_context::Grant::granted)
        detail::access_violation(typeid(T).name(), Access::read_write, g == Access_context::Grant::stale);
}

template<typename T>
inline void access_check(const T* self) noexcept
{
    const Access_context* ctx = detail::current_access;
    Access_context::Grant g = ctx ? ctx->check(self, Access::read_only) : Access_context::Grant::none;
    if (g != Access_context::Grant::granted)
        detail::access_violation(typeid(T).name(), Access::read_only, g == Access_context::Grant::stale);
}

#if TS_SAFETY_CHECKS
    #define TS_CHECK_ACCESS() ::ts::access_check(this)
#else
    #define TS_CHECK_ACCESS() ((void)0)
#endif

// Installed by the pump around each job; RAII save/restore so nested execution
// stacks contexts correctly.
class Access_scope
{
public:
    explicit Access_scope(const Access_context& ctx) noexcept
        : prev_(detail::current_access)
    {
        detail::current_access = &ctx;
    }

    ~Access_scope()
    {
        detail::current_access = prev_;
    }

    Access_scope(const Access_scope&) = delete;
    Access_scope& operator=(const Access_scope&) = delete;

private:
    const Access_context* prev_;
};

namespace detail
{

// Installs an inherited grant (a `snapshot_access()` copy) for the duration of a scope,
// if one was captured; a no-op when empty. `ctx` must outlive the scope -- in practice
// it is a by-value member of the launched task's body, alive for the whole call.
class Inherited_access_scope
{
public:
    explicit Inherited_access_scope(const std::optional<Access_context>& ctx) noexcept
        : active_(ctx.has_value())
    {
        if (active_)
        {
            prev_ = current_access;
            current_access = &*ctx;
        }
    }

    ~Inherited_access_scope()
    {
        if (active_)
            current_access = prev_;
    }

    Inherited_access_scope(const Inherited_access_scope&) = delete;
    Inherited_access_scope& operator=(const Inherited_access_scope&) = delete;

private:
    bool active_;
    const Access_context* prev_ = nullptr;
};

} // namespace detail

} // namespace ts
