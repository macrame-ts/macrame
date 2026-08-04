#include "rules_tests.h"
#include "harness.h"

#include "ts/coroutine_support.h"
#include "ts/rules.h"
#include "ts/task.h"

#include <atomic>
#include <thread>

using ts::Rule;
using ts::test::run;

namespace
{

// 1. Compile-out state. `await_under_guard` is the one rule a shipping build keeps by
// default (docs/waiting-rule-policy.md §2), so it is on in every configuration this suite
// builds in; the advisory rules track `TS_SAFETY_CHECKS`.
void test_compiled_in_set()
{
    // The runtime predicate must agree with the preprocessor mask the build was compiled
    // with -- that agreement is what makes `TS_RULE_ON` guards and `rule_compiled_in` two
    // spellings of one policy.
    TS_CHECK(ts::rule_compiled_in(Rule::await_under_guard) == (TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)));
    TS_CHECK(ts::rule_compiled_in(Rule::in_task_sync) == (TS_RULE_ON(TS_RULE_IN_TASK_SYNC)));
    TS_CHECK(ts::rule_compiled_in(Rule::waits_for_cycle) == (TS_RULE_ON(TS_RULE_WAITS_FOR_CYCLE)));
#if !TS_SAFETY_CHECKS
    // `waits_for_cycle` reads harness-only state, so the effective mask drops it regardless
    // of what the policy asked for.
    TS_CHECK(!ts::rule_compiled_in(Rule::waits_for_cycle));
#endif
}

// 2. The scoped opt-out relaxes exactly what it names, and restores on scope exit.
void test_relaxed_scope_scoping()
{
#if TS_RULES_ANY
    TS_CHECK(!ts::rule_relaxed(Rule::in_task_sync));
    {
        ts::Relaxed_scope relax{ Rule::in_task_sync };
        TS_CHECK(ts::rule_relaxed(Rule::in_task_sync));
        TS_CHECK(!ts::rule_relaxed(Rule::waits_for_cycle));   // not named -> still enforced
        {
            ts::Relaxed_scope inner{ Rule::waits_for_cycle };   // nests, does not replace
            TS_CHECK(ts::rule_relaxed(Rule::in_task_sync));
            TS_CHECK(ts::rule_relaxed(Rule::waits_for_cycle));
        }
        TS_CHECK(ts::rule_relaxed(Rule::in_task_sync));
        TS_CHECK(!ts::rule_relaxed(Rule::waits_for_cycle));
    }
    TS_CHECK(!ts::rule_relaxed(Rule::in_task_sync));
#endif
}

// 3. A non-advisory rule has no runtime opt-out: naming one is a diagnosed no-op for that
// bit (the escape for a structural rule is the sanctioned form, not a suppression).
void test_relaxed_scope_refuses_structural()
{
#if TS_SAFETY_CHECKS && TS_RULES_ANY
    long long before = ts::ensure_failure_count();
    ts::test::Expected_ensures expected(1);
    {
        ts::Relaxed_scope relax{ Rule::await_under_guard | Rule::in_task_sync };
        TS_CHECK(!ts::rule_relaxed(Rule::await_under_guard));   // structural bit dropped
        TS_CHECK(ts::rule_relaxed(Rule::in_task_sync));         // advisory bit honoured
    }
    TS_CHECK(ts::ensure_failure_count() == before + 1);
#endif
}

// 4. Reach, part 1: sub-work launched under a relaxation inherits it, exactly as it inherits
// the launcher's grant (docs/waiting-rule-policy.md §4).
void test_relaxed_scope_inherited_by_child()
{
#if TS_RULES_ANY
    std::atomic<bool> seen_in_child{ false };
    ts::Task<void> child;
    {
        ts::Relaxed_scope relax{ Rule::in_task_sync };
        child = ts::launch([&seen_in_child]
        {
            seen_in_child.store(ts::rule_relaxed(Rule::in_task_sync), std::memory_order_relaxed);
        });
    }
    child.sync();
    TS_CHECK(seen_in_child.load(std::memory_order_relaxed));
    TS_CHECK(!ts::rule_relaxed(Rule::in_task_sync));   // the launcher's thread is unaffected
#endif
}

// 4b. Reach, part 2: a relaxation opened in a coroutine body survives a genuine suspension
// and the resume on another thread, and does not leak onto the resuming thread.
ts::Task<bool> co_relaxed_across_suspension(ts::Signal& gate, std::atomic<bool>& leaked_on_resumer)
{
    ts::Relaxed_scope relax{ Rule::in_task_sync };
    co_await gate;   // resumes on whichever thread triggers the signal
    bool still_relaxed = ts::rule_relaxed(Rule::in_task_sync);
    co_return still_relaxed && !leaked_on_resumer.load(std::memory_order_relaxed);
}

void test_relaxed_scope_across_suspension()
{
#if TS_RULES_ANY
    ts::Signal gate;
    std::atomic<bool> leaked_on_resumer{ false };
    std::atomic<bool> left_behind{ false };
    ts::Task<bool> t = co_relaxed_across_suspension(gate, leaked_on_resumer);
    std::thread resumer([&gate, &leaked_on_resumer, &left_behind]
    {
        // The frame resumes inside `trigger()`, on this thread; record whether the
        // relaxation was visible here BEFORE the frame installed its own.
        leaked_on_resumer.store(ts::rule_relaxed(Rule::in_task_sync), std::memory_order_relaxed);
        gate.trigger();
        // And the frame's relaxation must not have been left behind on this thread.
        left_behind.store(ts::rule_relaxed(Rule::in_task_sync), std::memory_order_relaxed);
    });
    resumer.join();
    TS_CHECK(!left_behind.load(std::memory_order_relaxed));
    TS_CHECK(t.sync());
#endif
}

// 5. The process-wide default: OR-ed into every lookup, and masked to advisory rules like
// the scoped form.
void test_default_relaxed_rules()
{
    TS_CHECK(ts::default_relaxed_rules() == Rule::none);
    ts::set_default_relaxed_rules(Rule::all);
#if TS_RULES_ANY
    TS_CHECK(ts::rule_relaxed(Rule::in_task_sync));
    TS_CHECK(ts::rule_relaxed(Rule::access_rank));
    TS_CHECK(!ts::rule_relaxed(Rule::await_under_guard));   // structural: never relaxed
    TS_CHECK(!ts::rule_relaxed(Rule::deadlock_net));        // net: never relaxed
#endif
    ts::set_default_relaxed_rules(Rule::none);
    TS_CHECK(!ts::rule_relaxed(Rule::in_task_sync));
}

} // namespace

void run_rules_tests()
{
    run("rules compiled-in set", test_compiled_in_set);
    run("rules relaxed scope scoping", test_relaxed_scope_scoping);
    run("rules relaxed scope refuses structural", test_relaxed_scope_refuses_structural);
    run("rules relaxed scope inherited by child", test_relaxed_scope_inherited_by_child);
    run("rules relaxed scope across suspension", test_relaxed_scope_across_suspension);
    run("rules default relaxed set", test_default_relaxed_rules);
}
