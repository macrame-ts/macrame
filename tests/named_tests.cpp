#include "named_tests.h"
#include "harness.h"
#include "test_util.h"

#include "ts/guarded.h"
#include "ts/named.h"
#include "ts/static_task_graph.h"
#include "ts/task.h"
#include "ts/versioned.h"

#include <cstring>
#include <string>

using ts::Named;
using ts::test::run;

namespace
{

// True if `file` names this translation unit - the naming design ensures
// that a defaulted `source_location` captures the caller, so every site captured through a
// public verb must land here and never in `include/ts/...`.
bool is_this_file(const char* file)
{
    if (file == nullptr)
        return false;
    return std::string_view(file).ends_with("named_tests.cpp");
}

[[maybe_unused]] bool is_library_header(const char* file)
{
    if (file == nullptr)
        return false;
    std::string_view path(file);
    return path.find("include") != std::string_view::npos
        && (path.ends_with(".h") || path.ends_with(".hpp"));
}

// 1. The two spellings: a literal keeps the literal (and still records where it was
// written); `Named{}` records the site alone.
void test_named_forms()
{
    Named literal{ "physics" };
    TS_CHECK(literal.literal != nullptr && std::strcmp(literal.literal, "physics") == 0);
    TS_CHECK(is_this_file(literal.file));
    TS_CHECK(literal.line != 0);
    TS_CHECK(!literal.empty());

    Named site{};
    TS_CHECK(site.literal == nullptr);
    TS_CHECK(is_this_file(site.file));
    TS_CHECK(!site.empty());

    Named none{ nullptr };
    TS_CHECK(none.empty());
}

// 2. Display: the literal when there is one, else `basename:line`, else the fallback.
void test_named_display()
{
    char buf[128];
    Named literal{ "physics" };
    TS_CHECK(std::strcmp(ts::named_display(literal, buf, sizeof buf), "physics") == 0);

    Named site{};
    std::string shown = ts::named_display(site, buf, sizeof buf);
    TS_CHECK(shown.starts_with("named_tests.cpp:"));   // basename, not the full path
    TS_CHECK(shown.find('\\') == std::string::npos && shown.find('/') == std::string::npos);

    Named none{ nullptr };
    TS_CHECK(std::strcmp(ts::named_display(none, buf, sizeof buf, "<none>"), "<none>") == 0);
}

// 3. the guarantee: a task launched with no name is identified by its launch site in the
// caller's file. If any internal layer re-defaulted its own `source_location`, this would
// capture `include/ts/task.h` instead and the feature would be worthless.
void test_launch_captures_call_site()
{
#if TS_SAFETY_CHECKS
    ts::Task<int> t = ts::launch([] { return 7; });
    const Named& name = ts::detail::core_of(t)->name;
    TS_CHECK_MSG(is_this_file(name.file), "launch() must capture the CALLER's site");
    TS_CHECK(!is_library_header(name.file));
    TS_CHECK(name.literal == nullptr);
    TS_CHECK(t.sync() == 7);

    ts::Task<int> named = ts::launch([] { return 8; }, { .name = "answer" });
    const Named& explicit_name = ts::detail::core_of(named)->name;
    TS_CHECK(explicit_name.literal != nullptr && std::strcmp(explicit_name.literal, "answer") == 0);
    TS_CHECK(is_this_file(explicit_name.file));
    TS_CHECK(named.sync() == 8);
#endif
}

// 5. Through an access verb.
void test_access_verb_captures_call_site()
{
#if TS_SAFETY_CHECKS
    ts::Guarded<tests::Counter> counter{ Named{ "counter" } };
    ts::Task<void> t = counter.async([](tests::Counter& c) { c.increment(); });
    const Named& name = ts::detail::core_of(t)->name;
    TS_CHECK_MSG(is_this_file(name.file), "async() must capture the CALLER's site");
    TS_CHECK(!is_library_header(name.file));
    t.sync();

    ts::Task<void> tagged = counter.async([](tests::Counter& c) { c.increment(); }, { .name = "bump" });
    TS_CHECK(std::strcmp(ts::detail::core_of(tagged)->name.literal, "bump") == 0);
    tagged.sync();
#endif
}

// 5b. The multi-object verbs end in an object pack, so no defaulted `source_location` can
// follow them and the verb has no site to capture. `Access_options::name` is their whole
// identity - which is why it is a `ts::Named` rather than a literal: `{.name = ts::Named{}}`
// captures the caller's site by hand, and a literal names them outright.
void test_multi_object_access_name()
{
#if TS_SAFETY_CHECKS
    ts::Guarded<int> first{ Named{ "first" }, 2 };
    ts::Guarded<int> second{ Named{ "second" }, 3 };
    auto sited = ts::access({ .name = ts::Named{} },
                            [](const int& x, const int& y) { return x + y; }, first, second);
    const Named& name = ts::detail::access_op_core(sited)->name;
    TS_CHECK_MSG(is_this_file(name.file), "{.name = ts::Named{}} must capture the CALLER's site");
    TS_CHECK(!is_library_header(name.file));
    TS_CHECK(name.literal == nullptr);
    TS_CHECK(sited.sync() == 5);

    auto named = ts::access({ .name = "hud" },
                            [](const int& x, const int& y) { return x * y; }, first, second);
    TS_CHECK(std::strcmp(ts::detail::access_op_core(named)->name.literal, "hud") == 0);
    TS_CHECK(named.sync() == 6);
#endif
}

// 5c. `Versioned::publish` takes `Dispatch_options` and names the step it schedules: an
// explicit `.name` reaches the returned gate, and an unnamed publish falls back to the call
// site the verb captured (it dropped `opts.name` entirely before the M2 split).
void test_publish_carries_name()
{
#if TS_SAFETY_CHECKS
    ts::Versioned<int> versioned{ Named{ "scores" } };
    ts::Task<void> sited = versioned.publish();
    const Named& site_name = ts::detail::core_of(sited)->name;
    TS_CHECK_MSG(is_this_file(site_name.file), "publish() must capture the CALLER's site");
    TS_CHECK(!is_library_header(site_name.file));
    TS_CHECK(site_name.literal == nullptr);
    sited.sync();

    ts::Task<void> named = versioned.publish({ .name = "flip" });
    const Named& explicit_name = ts::detail::core_of(named)->name;
    TS_CHECK(explicit_name.literal != nullptr && std::strcmp(explicit_name.literal, "flip") == 0);
    named.sync();
#endif
}

// 6. A graph node's block carries the node's identity, so a diagnostic that names the
// task names the node - including the pipe entries the node takes, which are that block.
void test_node_block_carries_name()
{
#if TS_SAFETY_CHECKS
    ts::Guarded<int> value{ Named{ "value" }, 0 };
    ts::Static_task_graph graph;
    graph.add_node("bump", [](int& v) { v += 1; }, value);
    graph.add_node({}, [](const int&) {}, value);
    graph.compile();
    graph.execute().sync();

    // Reached through the public structure the trace consumes; the literal and the
    // site-captured node must both be identified.
    TS_CHECK(graph.node_count() == 2);
    TS_CHECK(value.async([](const int& v) { return v; }).sync() == 1);
#endif
}

// 7. An object constructed with `ts::Named{}` is identified by its construction site --
// the `Guarded` mirror of test 3.
void test_object_captures_construction_site()
{
    ts::Guarded<int> value{ Named{}, 0 };
    const Named& name = ts::detail::Guarded_access::pipe(value).debug_name;
    TS_CHECK(is_this_file(name.file));
    TS_CHECK(name.literal == nullptr);
}

} // namespace

void run_named_tests()
{
    run("named forms", test_named_forms);
    run("named display", test_named_display);
    run("named launch captures call site", test_launch_captures_call_site);
    run("named access verb captures call site", test_access_verb_captures_call_site);
    run("named multi-object access", test_multi_object_access_name);
    run("named publish carries name", test_publish_carries_name);
    run("named node block carries name", test_node_block_carries_name);
    run("named object captures construction site", test_object_captures_construction_site);
}
