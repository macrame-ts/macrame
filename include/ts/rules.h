#pragma once

// Rule policy -- which waiting-rule checks exist in this build, and how a program opts out
// of one for a scope. Design of record: docs/waiting-rule-policy.md.
//
// The waiting rules (docs/coroutine-first.md §2) are enforced by runtime checks that fatal.
// Each one can be wrong for a program that upholds the rule by means the library cannot see
// (an external lock discipline, a phase invariant, a platform guarantee), so every check
// answers two questions, decided per rule and stated in the table below:
//
//   1. is it compiled into this build?  -- `TS_ENABLED_RULES`, a bitmask of `TS_RULE_*`
//   2. is it enforced at this point?    -- `ts::Relaxed_scope`, for the rules that permit it
//
// Question 2 is deliberately not universal. A rule that protects an invariant the
// IMPLEMENTATION relies on (currently `await_under_guard`) has no sound runtime opt-out --
// relaxing it does not accept a program the library merely disapproves of, it produces one
// the library cannot execute correctly. Such rules are compile-out-only. See the doc's §3.
//
// Cost discipline: consult `rule_enforced` only AFTER the cheap hazard condition is already
// true. The relaxation lookup (one thread-local load, one relaxed atomic load) belongs on
// the branch that is about to fatal, never on the path every call takes.

#include "ts/fatal.h"

#include <atomic>

#ifndef TS_SAFETY_CHECKS
#define TS_SAFETY_CHECKS 1
#endif

// One bit per rule. Preprocessor values (not just enumerators) so `TS_ENABLED_RULES` can be
// tested with `#if` and a disabled rule's state and code vanish entirely.
#define TS_RULE_IN_TASK_SYNC      0x01
#define TS_RULE_AWAIT_UNDER_GUARD 0x02
#define TS_RULE_ACCESS_RANK       0x04
#define TS_RULE_WAITS_FOR_CYCLE   0x08
#define TS_RULE_DEADLOCK_NET      0x10
#define TS_RULE_ALL               0x1F

// The default policy. Checked builds enforce everything. A shipping build keeps only
// `await_under_guard`: it is the one rule whose absence corrupts rather than merely permits
// (see above), and it is what shipping already did before the policy existed. Override by
// defining `TS_ENABLED_RULES` to any OR of the bits -- one value per binary (below).
#ifndef TS_ENABLED_RULES
#if TS_SAFETY_CHECKS
#define TS_ENABLED_RULES TS_RULE_ALL
#else
#define TS_ENABLED_RULES TS_RULE_AWAIT_UNDER_GUARD
#endif
#endif

// Not every rule can be honoured in every configuration: `waits_for_cycle` reads grant
// bookkeeping (`Access_context` epochs, `Pipe::write_epoch`) that exists only under
// `TS_SAFETY_CHECKS`, so it cannot outlive it whatever the policy asks for. The EFFECTIVE
// mask is the policy intersected with what the build can support; everything downstream
// (including `rule_compiled_in`) reads the effective mask.
#if TS_SAFETY_CHECKS
#define TS_RULES_AVAILABLE TS_RULE_ALL
#else
#define TS_RULES_AVAILABLE (TS_RULE_ALL & ~TS_RULE_WAITS_FOR_CYCLE)
#endif

#define TS_RULES_EFFECTIVE ((TS_ENABLED_RULES) & (TS_RULES_AVAILABLE))
#define TS_RULE_ON(bit) (((TS_RULES_EFFECTIVE) & (bit)) != 0)
#define TS_RULES_ANY ((TS_RULES_EFFECTIVE) != 0)

// Same ODR hazard as `TS_SAFETY_CHECKS` (the macro changes inline bodies and the coroutine
// promise's layout), and the same best-effort tripwire. Note it compares the TOKEN TEXT, so
// two spellings of one value (`TS_RULE_ALL` vs `0x1F`) trip it: define the macro in exactly
// one place, as a build system does.
#if defined(_MSC_VER)
#define TS_DETAIL_RULES_STRINGIZE2(x) #x
#define TS_DETAIL_RULES_STRINGIZE(x) TS_DETAIL_RULES_STRINGIZE2(x)
#pragma detect_mismatch("TS_ENABLED_RULES", TS_DETAIL_RULES_STRINGIZE(TS_ENABLED_RULES))
#endif

namespace ts
{

// The rule set, mirroring the `TS_RULE_*` bits so one vocabulary serves the compile-time
// policy and the runtime opt-out.
//
// | rule                | what it forbids                              | ship | Relaxed_scope |
// |---------------------|----------------------------------------------|------|---------------|
// | `in_task_sync`      | `sync()`/`take()` inside a task              | out  | yes           |
// | `await_under_guard` | `co_await` while a `Pipe_guard` is live      | KEPT | no            |
// | `access_rank`       | awaiting an object out of declared rank order| out  | yes           |
// | `waits_for_cycle`   | a held-grant -> awaited-pipe wait cycle      | out  | yes           |
// | `deadlock_net`      | quiescence with no possible external wakeup  | out  | no (global)   |
enum class Rule : unsigned
{
    none = 0,

    in_task_sync = TS_RULE_IN_TASK_SYNC,
    await_under_guard = TS_RULE_AWAIT_UNDER_GUARD,
    access_rank = TS_RULE_ACCESS_RANK,
    waits_for_cycle = TS_RULE_WAITS_FOR_CYCLE,
    deadlock_net = TS_RULE_DEADLOCK_NET,

    // Classes. `advisory` rules encode a hazard the caller may know is absent, so they take
    // a scoped opt-out; `structural` rules protect an implementation invariant and are
    // compile-out-only; `net` is the global quiescence check, which cannot be scoped at all
    // because it fires on the whole process rather than at a call site.
    advisory = in_task_sync | access_rank | waits_for_cycle,
    structural = await_under_guard,
    net = deadlock_net,
    all = TS_RULE_ALL,
};

constexpr Rule operator|(Rule a, Rule b) noexcept
{
    return static_cast<Rule>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

constexpr Rule operator&(Rule a, Rule b) noexcept
{
    return static_cast<Rule>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
}

constexpr Rule operator~(Rule a) noexcept
{
    return static_cast<Rule>(~static_cast<unsigned>(a) & static_cast<unsigned>(Rule::all));
}

constexpr bool any(Rule r) noexcept { return r != Rule::none; }

// Whether `rule` is compiled into this build at all (policy AND what the build supports).
constexpr bool rule_compiled_in(Rule rule) noexcept
{
    return any(rule & static_cast<Rule>(TS_RULES_EFFECTIVE));
}

namespace detail
{

#if TS_RULES_ANY
// Rules relaxed for the running scope. Thread-local, but carried with the ambient task state
// rather than with the thread: a coroutine's segments re-install it (`Relaxed_carrier`) and
// launched sub-work inherits it (`Relaxed_snapshot`), exactly as grants propagate.
inline thread_local unsigned relaxed_rules = 0;
// Process-wide baseline, OR-ed into every lookup ("the rules as advice" setting).
inline std::atomic<unsigned> default_relaxed_rules_bits{ 0 };
#endif

} // namespace detail

// True if `rule` is currently opted out of -- by an enclosing `Relaxed_scope` or by the
// process-wide default. Non-advisory rules never report relaxed.
inline bool rule_relaxed(Rule rule) noexcept
{
#if TS_RULES_ANY
    unsigned bits = detail::relaxed_rules
        | detail::default_relaxed_rules_bits.load(std::memory_order_relaxed);
    return any(static_cast<Rule>(bits) & rule & Rule::advisory);
#else
    (void)rule;
    return false;
#endif
}

// The one predicate a rule check calls: is this rule compiled in AND not relaxed here?
// Call it only once the hazard condition itself is known true (see the header comment).
inline bool rule_enforced(Rule rule) noexcept
{
    return rule_compiled_in(rule) && !rule_relaxed(rule);
}

// Process-wide relaxation baseline, for teams that want the advisory rules as advice.
// Non-advisory bits are ignored (they have no sound runtime opt-out). Set once at startup;
// read relaxed, so a change is not ordered against concurrently running tasks.
inline void set_default_relaxed_rules(Rule rules) noexcept
{
#if TS_RULES_ANY
    detail::default_relaxed_rules_bits.store(static_cast<unsigned>(rules & Rule::advisory),
                                             std::memory_order_relaxed);
#else
    (void)rules;
#endif
}

inline Rule default_relaxed_rules() noexcept
{
#if TS_RULES_ANY
    return static_cast<Rule>(detail::default_relaxed_rules_bits.load(std::memory_order_relaxed));
#else
    return Rule::none;
#endif
}

// Opt out of one or more ADVISORY rules for a scope -- "I uphold this rule by means the
// library cannot see". Documents the claim at the site and leaves the rest of the program
// checked. The opt-out follows the ambient task state, not the thread: it is re-installed
// around every segment of a coroutine that entered it, and inherited by sub-work launched
// under it (`ts::launch` / `ts::nested`) -- wider than the lexical scope, and deliberately
// so, since a child inherits the grant and therefore the hazard.
//
// Passing a non-advisory rule is a diagnosed no-op for those bits (`TS_ENSURE`): the escape
// for a structural rule is the sanctioned form, and for the quiescence net it is
// `ts::External_wait`.
class Relaxed_scope
{
public:
    explicit Relaxed_scope(Rule rules) noexcept
    {
        TS_ENSURE(!any(rules & ~Rule::advisory),
            "ts::Relaxed_scope: only advisory rules can be relaxed at runtime; a structural "
            "rule is compile-out-only (TS_ENABLED_RULES) and the quiescence net is global");
#if TS_RULES_ANY
        prev_ = detail::relaxed_rules;
        detail::relaxed_rules = prev_ | static_cast<unsigned>(rules & Rule::advisory);
#endif
    }

    ~Relaxed_scope()
    {
#if TS_RULES_ANY
        detail::relaxed_rules = prev_;
#endif
    }

    Relaxed_scope(const Relaxed_scope&) = delete;
    Relaxed_scope& operator=(const Relaxed_scope&) = delete;

private:
#if TS_RULES_ANY
    unsigned prev_ = 0;
#endif
};

namespace detail
{

// Segment carriage for a coroutine promise. A `Relaxed_scope` entered in a coroutine body
// must survive the body's suspensions, and must not leak onto whatever thread resumes it:
// the promise holds the frame's relaxation, `enter` installs it and `exit` writes back
// whatever the segment ended with. Restoring a saved VALUE is thread-agnostic, so a segment
// resumed on another worker restores correctly.
struct Relaxed_carrier
{
#if TS_RULES_ANY
    unsigned bits = relaxed_rules;   // the creator's relaxation
    unsigned prev = 0;

    void enter() noexcept
    {
        prev = relaxed_rules;
        relaxed_rules = bits;
    }

    void exit() noexcept
    {
        bits = relaxed_rules;
        relaxed_rules = prev;
    }
#else
    void enter() noexcept {}
    void exit() noexcept {}
#endif
};

// By-value capture of the launcher's relaxation, for sub-work that inherits its grant
// (`with_inherited_access`). Empty when no rule is compiled in.
struct Relaxed_snapshot
{
#if TS_RULES_ANY
    unsigned bits = relaxed_rules;
#endif
};

// Installs a `Relaxed_snapshot` for the duration of a scope (the inherited-grant analogue of
// `Inherited_access_scope`).
class Inherited_relaxed_scope
{
public:
    explicit Inherited_relaxed_scope(const Relaxed_snapshot& snapshot) noexcept
    {
#if TS_RULES_ANY
        prev_ = relaxed_rules;
        relaxed_rules = snapshot.bits;
#else
        (void)snapshot;
#endif
    }

    ~Inherited_relaxed_scope()
    {
#if TS_RULES_ANY
        relaxed_rules = prev_;
#endif
    }

    Inherited_relaxed_scope(const Inherited_relaxed_scope&) = delete;
    Inherited_relaxed_scope& operator=(const Inherited_relaxed_scope&) = delete;

private:
#if TS_RULES_ANY
    unsigned prev_ = 0;
#endif
};

} // namespace detail

} // namespace ts
