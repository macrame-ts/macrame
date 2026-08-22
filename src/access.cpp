#include "ts/access.h"
#include "ts/fatal.h"

#include <cstdio>

namespace ts
{

namespace detail
{

// The link-time config tripwire's anchor (see access.h): exactly one of the two names
// exists per library build, so a TU compiled with the other `TS_SAFETY_CHECKS` value
// fails to link with an unresolved symbol naming the mismatch.
#if TS_SAFETY_CHECKS
const char config_safety_checks_on = 1;
#else
const char config_safety_checks_off = 1;
#endif

#if TS_SUSPENSION_REGISTRY
const char config_suspension_registry_on = 1;
#else
const char config_suspension_registry_off = 1;
#endif

#if TS_DEBUG_NAMES
const char config_debug_names_on = 1;
#else
const char config_debug_names_off = 1;
#endif

} // namespace detail

void Access_context::add(const void* instance, Access mode,
                         [[maybe_unused]] const std::atomic<std::uint64_t>* epoch,
                         [[maybe_unused]] unsigned rank) noexcept
{
    // Silent truncation is a latent footgun, in every build: under the harness a lost
    // declaration makes a later legitimate access fault spuriously in `check` (a false
    // positive far from the cause), and even with the harness off the nested-run lend
    // decision (`bind_links_for_run`) consults the context - a truncated entry there
    // turns into a shipping-only deadlock (the inner node queues behind the caller's
    // unlent hold). Fail loud at the declaration, unconditionally - a cold path.
    if (count_ == max_entries)
    {
        char message[160];
        std::snprintf(message, sizeof message,
            "Access_context overflow: more than %d declared objects in one task; "
            "raise Access_context::max_entries", max_entries);
        ts::fatal(message);
    }
#if TS_SAFETY_CHECKS
    entries_[count_++] = { instance, mode, epoch,
                           epoch ? epoch->load(std::memory_order_relaxed) : 0
#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
                           , rank
#endif
                         };
#else
    // Staleness is a harness diagnostic; without the harness an entry is address + mode.
    entries_[count_++] = { instance, mode };
#endif
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
        // Keep scanning - a duplicate entry for the same object may still be current.
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

void access_violation(const char* type_name, Access mode, bool stale) noexcept
{
    char message[256];
    if (stale)
    {
        std::snprintf(message, sizeof message,
            "access violation: %s accessed for %s under a stale inherited grant "
            "(the task outlived the access scope it inherited from; acquire fresh via "
            "obj.async / co_await obj.access, or fan out with ts::parallel_for)",
            type_name, mode == Access::read_write ? "read_write" : "read_only");
    }
    else
    {
        std::snprintf(message, sizeof message,
            "access violation: %s accessed for %s without declared access",
            type_name, mode == Access::read_write ? "read_write" : "read_only");
    }
    ts::fatal(message);
}

} // namespace detail
} // namespace ts
