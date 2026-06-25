#include "access.h"

#include <cstdio>
#include <cstdlib>

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

        // read_only is satisfied by any held mode; read_write needs read_write.
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
    std::fprintf(stderr,
        "ts: access violation: %s accessed for %s without declared access\n",
        type_name, mode == Access::read_write ? "read_write" : "read_only");
    std::abort();
}

} // namespace detail
} // namespace ts
