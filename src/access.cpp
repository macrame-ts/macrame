#include "ts/access.h"
#include "ts/fatal.h"

#include <cstdio>

namespace ts
{

void Access_context::add(const void* instance, Access mode,
                         const std::atomic<std::uint64_t>* epoch) noexcept
{
#if TS_SAFETY_CHECKS
    // Silent truncation is a latent footgun: a task declaring more than `max_entries`
    // objects would lose the overflowing declaration, and a later legitimate access to
    // that object then faults spuriously in `check` -- a false positive surfacing far
    // from the cause. Fail loud at the declaration instead.
    if (count_ == max_entries)
        ts::fatal("Access_context overflow: more than 8 declared objects in one task; "
            "raise Access_context::max_entries");
#else
    epoch = nullptr;   // staleness is a harness diagnostic; skip the capture when compiled out
#endif
    if (count_ < max_entries)
        entries_[count_++] = { instance, mode, epoch,
                               epoch ? epoch->load(std::memory_order_relaxed) : 0 };
}

Access_context::Grant Access_context::check(const void* instance, Access mode) const noexcept
{
    bool stale = false;
    for (int i = 0; i < count_; ++i)
    {
        const Entry& e = entries_[i];
        if (e.instance != instance)
            continue;

        // `read_only` is satisfied by any held mode; `read_write` needs `read_write`.
        if (mode == Access::read_write && e.mode != Access::read_write)
            continue;

#if TS_SAFETY_CHECKS
        // Grant-window validity: the pipe's write-epoch moved on -> this entry's window is
        // over (the write holder released, or a writer acquired after a read capture).
        // Keep scanning -- a duplicate entry for the same object may still be current.
        if (e.epoch && e.epoch->load(std::memory_order_relaxed) != e.captured)
        {
            stale = true;
            continue;
        }
#endif
        return Grant::granted;
    }
    return stale ? Grant::stale : Grant::none;
}

namespace detail
{

thread_local const Access_context* current_access = nullptr;

void access_violation(const char* type_name, Access mode, bool stale) noexcept
{
    char message[256];
    if (stale)
        std::snprintf(message, sizeof message,
            "access violation: %s accessed for %s under a stale inherited grant "
            "(the task outlived the access scope it inherited from; gate it with "
            "ts::nested / ts::add_nested)",
            type_name, mode == Access::read_write ? "read_write" : "read_only");
    else
        std::snprintf(message, sizeof message,
            "access violation: %s accessed for %s without declared access",
            type_name, mode == Access::read_write ? "read_write" : "read_only");
    ts::fatal(message);
}

} // namespace detail
} // namespace ts
