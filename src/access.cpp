#include "ts/access.h"
#include "ts/fatal.h"

#include <cstdio>

namespace ts
{

void Access_context::add(const void* instance, Access mode) noexcept
{
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
