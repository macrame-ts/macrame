#include "ts/access.h"
#include "ts/fatal.h"

#include <cstdio>

namespace ts
{

void Access_context::add(const void* instance, Access mode) noexcept
{
#if TS_SAFETY_CHECKS
    // Silent truncation is a latent footgun: a task declaring more than `max_entries`
    // objects would lose the overflowing declaration, and a later legitimate access to
    // that object then faults spuriously in `grants` -- a false positive surfacing far
    // from the cause. Fail loud at the declaration instead.
    if (count_ == max_entries)
        ts::fatal("Access_context overflow: more than 8 declared objects in one task; "
            "raise Access_context::max_entries");
#endif
    if (count_ < max_entries)
        entries_[count_++] = { instance, mode };
}

bool Access_context::grants(const void* instance, Access mode) const noexcept
{
    for (int i = 0; i < count_; ++i)
    {
        if (entries_[i].instance != instance)
            continue;

        // `read_only` is satisfied by any held mode; `read_write` needs `read_write`.
        if (mode == Access::read_only || entries_[i].mode == Access::read_write)
            return true;
    }
    return false;
}

namespace detail
{

thread_local const Access_context* current_access = nullptr;

void access_violation(const char* type_name, Access mode) noexcept
{
    char message[256];
    std::snprintf(message, sizeof message,
        "access violation: %s accessed for %s without declared access",
        type_name, mode == Access::read_write ? "read_write" : "read_only");
    ts::fatal(message);
}

} // namespace detail
} // namespace ts
