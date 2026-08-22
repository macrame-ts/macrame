#include "rules_tests.h"
#include "harness.h"

#include "ts/coroutine_support.h"
#include "ts/detail/suspension_registry.h"
#include "ts/rules.h"
#include "ts/static_task_graph.h"
#include "ts/task.h"

#include <chrono>
#include <atomic>
#include <thread>

using ts::Rule;
using ts::test::run;
using namespace ts::test;

namespace
{

// 1. Compile-out state. `await_under_guard` is the one rule a shipping build keeps by
// default (docs/waiting-rule-policy.md §2), so it is on in every configuration this suite
// builds in; the advisory rules track `TS_SAFETY_CHECKS`.
void test_compiled_in_set()
{
    // The runtime predicate must agree with the preprocessor mask the build was compiled
    // with - that agreement is what makes `TS_RULE_ON` guards and `rule_compiled_in` two
    // spellings of one policy.
    TS_CHECK(ts::rule_compiled_in(Rule::await_under_guard) == (TS_RULE_ON(TS_RULE_AWAIT_UNDER_GUARD)));
    TS_CHECK(ts::rule_compiled_in(Rule::in_task_sync) == (TS_RULE_ON(TS_RULE_IN_TASK_SYNC)));
    TS_CHECK(ts::rule_compiled_in(Rule::circular_wait) == (TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)));
#if !TS_SAFETY_CHECKS
    // `circular_wait` reads harness-only state, so the effective mask drops it regardless
    // of what the policy asked for.
    TS_CHECK(!ts::rule_compiled_in(Rule::circular_wait));
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
        TS_CHECK(!ts::rule_relaxed(Rule::circular_wait));   // not named -> still enforced
        {
            ts::Relaxed_scope inner{ Rule::circular_wait };   // nests, does not replace
            TS_CHECK(ts::rule_relaxed(Rule::in_task_sync));
            TS_CHECK(ts::rule_relaxed(Rule::circular_wait));
        }
        TS_CHECK(ts::rule_relaxed(Rule::in_task_sync));
        TS_CHECK(!ts::rule_relaxed(Rule::circular_wait));
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

// 4. Reach, part 1: a relaxation opened in a task body does not leak into a detached
// `ts::launch`. A detached child's handle may outlive the launcher's scope, so it runs as a
// fresh context on its own worker and sees no relaxation (docs/coroutine-first.md §2). The
// grant-inheriting form that does carry the relaxation is a coroutine segment - part 2 below.
void test_relaxed_scope_inherited_by_child()
{
#if TS_RULES_ANY
    std::atomic<bool> seen_in_detached{ true };   // detached must not inherit -> ends false
    ts::Signal detached_done;
    ts::launch([&]
    {
        ts::Relaxed_scope relax{ Rule::in_task_sync };
        // Detached child: a fresh context, so the relaxation does not reach it.
        ts::launch([&seen_in_detached, &detached_done]
        {
            seen_in_detached.store(ts::rule_relaxed(Rule::in_task_sync), std::memory_order_relaxed);
            detached_done.trigger();
        });
    }).sync();
    detached_done.sync();             // the detached child is not gated - wait for it explicitly
    TS_CHECK(!seen_in_detached.load(std::memory_order_relaxed));   // detached -> fresh context
    TS_CHECK(!ts::rule_relaxed(Rule::in_task_sync));               // the test thread is unaffected
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
        // relaxation was visible here before the frame installed its own.
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

// 4c. Reach, part 3: the relaxation must be read from the RESUMING thread's thread-local
// block. A guard living across a suspension invites the compiler to materialize the
// thread-local's address once, in the coroutine frame, and reuse it after the resume - which
// is the suspending thread's block. MSVC (19.51) does exactly that when `Relaxed_scope`'s
// constructor is inlined into the frame, so an inlined read of the relaxation returned the
// value the frame was suspended with while an out-of-line read of the same variable returned
// the live one. The two must agree; disagreement means a stale block, and every rule check
// (plus anything else the body reads out of a thread-local) is answering for the wrong thread.
#if TS_RULES_ANY
#if defined(_MSC_VER) && !defined(__clang__)
#define TS_TEST_NOINLINE __declspec(noinline)
#else
#define TS_TEST_NOINLINE [[gnu::noinline]]
#endif

TS_TEST_NOINLINE unsigned relaxed_bits_out_of_line()
{
    return ts::detail::relaxed_load();
}

TS_TEST_NOINLINE bool rule_relaxed_out_of_line()
{
    return ts::rule_relaxed(Rule::in_task_sync);
}

// Reads the same thread-local on both sides of the suspension, which is what makes the
// compiler want to keep the block pointer in the frame.
ts::Task<bool> co_relaxed_tls_freshness(ts::Signal& gate, std::atomic<unsigned>& before,
                                        std::atomic<unsigned>& after)
{
    ts::Relaxed_scope relax{ Rule::in_task_sync };
    before.store(ts::detail::relaxed_load(), std::memory_order_relaxed);
    co_await gate;
    unsigned inlined_bits = ts::detail::relaxed_load();
    bool inlined_predicate = ts::rule_relaxed(Rule::in_task_sync);
    after.store(inlined_bits, std::memory_order_relaxed);
    co_return inlined_bits == relaxed_bits_out_of_line()
        && inlined_predicate == rule_relaxed_out_of_line()
        && inlined_predicate;
}
#endif

void test_relaxed_scope_tls_freshness()
{
#if TS_RULES_ANY
    ts::Signal gate;
    std::atomic<unsigned> before{ 0 };
    std::atomic<unsigned> after{ 0 };
    ts::Task<bool> t = co_relaxed_tls_freshness(gate, before, after);
    std::thread resumer([&gate] { gate.trigger(); });
    resumer.join();
    TS_CHECK(before.load(std::memory_order_relaxed) != 0);   // the scope did take effect
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

// 6. Declared rank (`Rule::access_rank`, TODO 6.14). The two rejected shapes have death
// tests (`access_rank_descends`, `access_rank_unranked`); here are the sanctioned ones.
#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
ts::Task<void> climb(ts::Guarded<int>& higher, std::atomic<int>& seen)
{
    seen.store(co_await higher.access([](const int& v) { return v; }), std::memory_order_relaxed);
}
#endif

// Sanctioned form 1: the awaited object outranks everything held, so the await climbs and
// Havender's argument makes a cycle through these objects unrepresentable.
void test_access_rank_climb()
{
#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
    ts::Guarded<int> low{ ts::Named{ "low" }, ts::Rank{ 10 }, 0 };
    ts::Guarded<int> high{ ts::Named{ "high" }, ts::Rank{ 20 }, 7 };
    std::atomic<int> seen{ -1 };
    ts::Static_task_graph graph;
    graph.add_node("climber", [&high, &seen](int& own) -> ts::Task<void>
    {
        own = 1;
        co_await climb(high, seen);
    }, low);
    graph.compile();
    graph.execute().sync();
    TS_CHECK(seen.load(std::memory_order_relaxed) == 7);
#endif
}

// Sanctioned form 2: the per-scope opt-out, for a program that upholds the order by means
// the library cannot see. Note it is advisory, so a `Relaxed_scope` really does suppress it
// - unlike the structural guard rule.
void test_access_rank_relaxed()
{
#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
    ts::Guarded<int> high{ ts::Named{ "high" }, ts::Rank{ 30 }, 0 };
    ts::Guarded<int> low{ ts::Named{ "low" }, ts::Rank{ 10 }, 5 };
    std::atomic<int> seen{ -1 };
    ts::Static_task_graph graph;
    graph.add_node("descend-but-declared", [&low, &seen](int& own) -> ts::Task<void>
    {
        own = 1;
        ts::Relaxed_scope relax{ ts::Rule::access_rank };
        seen.store(co_await low.access([](const int& v) { return v; }), std::memory_order_relaxed);
    }, high);
    graph.compile();
    graph.execute().sync();
    TS_CHECK(seen.load(std::memory_order_relaxed) == 5);
#endif
}

void test_death_access_rank()
{
#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
    TS_CHECK(ts::test::expect_death("access_rank_descends"));
    TS_CHECK(ts::test::expect_death("access_rank_unranked"));
#endif
}

// 7. The deadlock net (`Rule::deadlock_net`). It fires from a blocked boundary waiter when
// the scheduler has been continuously quiescent with nothing registered as externally
// completable - see the `deadlock_net` death scenario. Here: the escape.
#if TS_RULE_ON(TS_RULE_DEADLOCK_NET)
// Restores the process-wide window whatever the test does.
struct Net_window
{
    explicit Net_window(std::chrono::milliseconds window) { ts::set_deadlock_net_window(window); }
    ~Net_window() { ts::set_deadlock_net_window(std::chrono::milliseconds(2000)); }
};
#endif

// A wait that only a non-worker thread can complete is legitimate, not a deadlock - as long
// as it is declared. `ts::External_wait` is that declaration, and the net's fatal names it
// because a forgotten one reports a correct program as deadlocked.
void test_external_wait_suppresses_net()
{
#if TS_RULE_ON(TS_RULE_DEADLOCK_NET)
    Net_window window(std::chrono::milliseconds(150));
    ts::launch([] {}).sync();   // let the pool go quiet

    ts::Signal from_outside;
    std::thread waker([from_outside]() mutable
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));   // well past the window
        from_outside.trigger();
    });
    {
        ts::External_wait declared;   // "a thread we do not own will complete this"
        from_outside.sync();          // quiescent the whole time, and correctly not reported
    }
    waker.join();
    TS_CHECK(from_outside.is_done());
#endif
}

// The whole-process off switch, for a program whose blue-thread handoffs the net cannot
// model. `TS_ENABLED_RULES` is the whole-build equivalent.
void test_deadlock_net_window_disable()
{
#if TS_RULE_ON(TS_RULE_DEADLOCK_NET)
    Net_window window(std::chrono::milliseconds(0));
    ts::launch([] {}).sync();

    ts::Signal from_outside;
    std::thread waker([from_outside]() mutable
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        from_outside.trigger();
    });
    from_outside.sync();   // no External_wait, no net: disabled
    waker.join();
    TS_CHECK(from_outside.is_done());
#endif
}

void test_death_deadlock_net()
{
#if TS_RULE_ON(TS_RULE_DEADLOCK_NET)
    TS_CHECK(ts::test::expect_death("deadlock_net"));
    // The shape only tier 3 can see: a task suspended on a plain task await while holding a
    // grant, so no wait edge exists. The child's report names it (verified by eye; the
    // harness has no fatal-output capture) - what this asserts is that the net still fires.
    TS_CHECK(ts::test::expect_death("deadlock_net_suspended"));
#endif
}

// 8. The suspension registry itself (tier 3): a live suspension is registered, and the entry
// is retired when the frame resumes - the property the whole report depends on.
#if TS_SUSPENSION_REGISTRY
ts::Task<void> park_on(ts::Signal gate)
{
    co_await gate;
}
#endif

void test_suspension_registry_tracks_live_suspensions()
{
#if TS_SUSPENSION_REGISTRY
    const int before = ts::detail::suspension_count();
    ts::Signal gate{ "registry_gate" };
    ts::Task<void> parked = park_on(gate);
    // The coroutine is eager, so it has already reached the await and suspended.
    TS_CHECK(ts::detail::suspension_count() == before + 1);
    gate.trigger();
    parked.sync();
    TS_CHECK(ts::detail::suspension_count() == before);
#else
    TS_CHECK(ts::detail::suspension_count() == -1);   // compiled out: "not tracked"
#endif
}

} // namespace

void run_rules_tests()
{
    run("rules compiled-in set", test_compiled_in_set);
    run("rules relaxed scope scoping", test_relaxed_scope_scoping);
    run("rules relaxed scope refuses structural", test_relaxed_scope_refuses_structural);
    run("rules relaxed scope inherited by child", test_relaxed_scope_inherited_by_child);
    run("rules relaxed scope across suspension", test_relaxed_scope_across_suspension);
    run("rules relaxed scope reads the resuming thread", test_relaxed_scope_tls_freshness);
    run("rules default relaxed set", test_default_relaxed_rules);
    run("rules access rank climb", test_access_rank_climb);
    run("rules access rank relaxed", test_access_rank_relaxed);
    run_if(with_rule_access_rank, "TS_RULE_ACCESS_RANK off", "death: access rank", test_death_access_rank);
    run("rules external wait suppresses the net", test_external_wait_suppresses_net);
    run("rules deadlock net window disable", test_deadlock_net_window_disable);
    run_if(with_rule_deadlock_net, "TS_RULE_DEADLOCK_NET off", "death: deadlock net", test_death_deadlock_net);
    run("suspension registry tracks live suspensions", test_suspension_registry_tracks_live_suspensions);
}
