#pragma once

#include <optional>
#include <typeinfo>

#ifndef TS_SAFETY_CHECKS
#define TS_SAFETY_CHECKS 1
#endif

namespace ts
{

enum class Access { read_only, read_write };

// Per-task permission set, installed thread-locally while a task runs. Small by
// design: a task touches a handful of instances, so an inline array + linear
// scan beats a hash set. Identity of an instance is its address.
class Access_context
{
public:
    void add(const void* instance, Access mode) noexcept;
    bool grants(const void* instance, Access mode) const noexcept;

private:
    static constexpr int max_entries = 8;

    struct Entry
    {
        const void* instance;
        Access mode;
    };

    Entry entries_[max_entries];
    int count_ = 0;
};

namespace detail
{

// null when this thread is not executing a task -> any guarded access faults.
extern thread_local const Access_context* current_access;

[[noreturn]] void access_violation(const char* type_name, Access mode) noexcept;

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
    if (!ctx || !ctx->grants(self, Access::read_write))
        detail::access_violation(typeid(T).name(), Access::read_write);
}

template<typename T>
inline void access_check(const T* self) noexcept
{
    const Access_context* ctx = detail::current_access;
    if (!ctx || !ctx->grants(self, Access::read_only))
        detail::access_violation(typeid(T).name(), Access::read_only);
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
