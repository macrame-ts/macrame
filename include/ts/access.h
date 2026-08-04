#pragma once

#include "ts/named.h"
#include "ts/rules.h"   // TS_RULE_ON: the rank bookkeeping below is `Rule::access_rank`'s

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

// Same treatment for the two diagnostics switches that also change class layout (the
// control block's `Named`, the awaiters' embedded registry record). MSVC catches these via
// `detect_mismatch` in rules.h; elsewhere the anchor symbol is the mechanism.
#if TS_SUSPENSION_REGISTRY
extern const char config_suspension_registry_on;
inline const char* const config_registry_tripwire = &config_suspension_registry_on;
#else
extern const char config_suspension_registry_off;
inline const char* const config_registry_tripwire = &config_suspension_registry_off;
#endif

#if TS_DEBUG_NAMES
extern const char config_debug_names_on;
inline const char* const config_names_tripwire = &config_debug_names_on;
#else
extern const char config_debug_names_off;
inline const char* const config_names_tripwire = &config_debug_names_off;
#endif

} // namespace detail

enum class Access { read_only, read_write };

// A guarded object's LOCK RANK, for `Rule::access_rank` (TODO 6.14). Batch acquisition is
// already deadlock-free -- a node's declared set and a multi-object `ts::access` are taken
// in canonical pipe-address order, all-or-nothing. What nothing orders is a grant a task
// already HOLDS against an object it awaits LATER, and that single missing constraint is
// the whole suspended-ABBA hole. A rank closes it by Havender's argument: every dynamic
// await must climb, so a cycle is unrepresentable.
//
//   ts::Guarded<Nav> nav{ ts::Named{"nav"}, ts::Rank{ 20 } };
//
// Ranks start at 1; **0 means "unranked", and unranked is the strict default**: a task
// holding an unranked grant may not dynamically await at all, and an unranked object may not
// be dynamically awaited while anything is held. Rank is deliberately NOT defaulted to
// address order -- that would make rejection ABI-dependent and irreproducible across builds,
// so a program that compiled and ran today could be rejected tomorrow for no source change.
// Only objects that are actually awaited need one.
struct Rank
{
    constexpr explicit Rank(unsigned value) noexcept : value(value) {}
    unsigned value;
};

// Per-task permission set, installed thread-locally while a task runs. Small by
// design: a task touches a handful of instances, so an inline array + linear
// scan beats a hash set. Identity of an instance is its address.
//
// An entry may carry a GRANT-VALIDITY source: a pointer to the object's
// `write_epoch` plus the value captured when the grant was declared. The epoch has
// seqlock-style parity -- bumped at every write-grant acquire and release (and by +2 on a
// graph write handoff) -- so "epoch unchanged" means the grant window this entry was
// declared under is still the object's current one: a write holder's window is still open,
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
    void add(const void* instance, Access mode, const std::atomic<std::uint64_t>* epoch,
             unsigned rank = 0) noexcept;
    Grant check(const void* instance, Access mode) const noexcept;
    bool grants(const void* instance, Access mode) const noexcept
    {
        return check(instance, mode) == Grant::granted;
    }

#if TS_SAFETY_CHECKS
    // Whether any entry's grant was declared under the given epoch source (i.e. this
    // context holds a grant on that object). Consumed by the blocking-sync diagnostic,
    // which compares a sync target against the caller's held grant sources; entries
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

    // As `holds_epoch`, but only a `read_write` entry matches -- "this scope holds the
    // WRITE grant whose window `epoch` tracks". Distinguishes an inherited/parent write
    // grant from a mere read grant (`Deferred::commit`'s misuse diagnostic).
    bool holds_write_epoch(const std::atomic<std::uint64_t>* epoch) const noexcept
    {
        for (int i = 0; i < count_; ++i)
        {
            if (entries_[i].epoch == epoch && entries_[i].mode == Access::read_write)
                return true;
        }
        return false;
    }

    // Invoke `fn(epoch)` for every entry that carries a grant-validity source -- i.e. every
    // pipe-backed grant this context holds. Consumed by the waits-for cycle detector, which
    // records held-grant -> awaited-pipe edges at suspension (docs/coroutine-first.md §2).
    template<typename Fn>
    void for_each_epoch(Fn&& fn) const noexcept
    {
        for (int i = 0; i < count_; ++i)
        {
            if (entries_[i].epoch != nullptr)
                fn(entries_[i].epoch);
        }
    }
#endif

#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
    // Held-side input to the rank check (`Rule::access_rank`). Only PIPE-BACKED entries
    // count -- a grant-free entry (a `Versioned` shadow, a hand-built context) excludes
    // nobody, so it constrains no later await. Returns the number of such entries, the
    // highest rank among them, and whether any of them is unranked (rank 0), which by
    // itself forbids a dynamic await: an object nobody gave an order cannot be climbed away
    // from safely.
    struct Rank_state
    {
        int held = 0;
        unsigned max = 0;
        bool any_unranked = false;
    };

    Rank_state rank_state() const noexcept
    {
        Rank_state state;
        for (int i = 0; i < count_; ++i)
        {
            if (entries_[i].epoch == nullptr)
                continue;
            // A grant whose window has closed excludes nobody, so it constrains no later
            // await -- the same staleness test `check` applies. This is what keeps a
            // detached coroutine, which carries its launcher's grant snapshot forever, from
            // being treated as a holder after the launcher released.
            if (entries_[i].epoch->load(std::memory_order_relaxed) != entries_[i].captured)
                continue;
            ++state.held;
            if (entries_[i].rank == 0)
                state.any_unranked = true;
            else if (entries_[i].rank > state.max)
                state.max = entries_[i].rank;
        }
        return state;
    }
#endif

private:
    static constexpr int max_entries = 8;

    struct Entry
    {
        const void* instance;
        Access mode;
#if TS_SAFETY_CHECKS
        const std::atomic<std::uint64_t>* epoch;   // null = never stale (and not pipe-backed)
        std::uint64_t captured;
#endif
#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
        unsigned rank;   // the object's declared `ts::Rank`; 0 = unranked
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
