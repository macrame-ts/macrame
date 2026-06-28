#include "thread_safe_tests.h"
#include "thread_safe.h"
#include "static_task_graph.h"
#include "harness.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using ts::test::run;

namespace
{

// A thread-unsafe "system": encapsulated, every public method guarded.
class Counter
{
public:
    void increment()
    {
        TS_CHECK_ACCESS();   // non-const -> read_write
        ++value_;
    }

    int value() const
    {
        TS_CHECK_ACCESS();   // const -> read_only
        return value_;
    }

private:
    int value_ = 0;
};

struct Probe
{
    std::atomic<int> active{ 0 };
    std::atomic<int> max_readers{ 0 };
    std::atomic<bool> writing{ false };
    std::atomic<bool> violated{ false };
};

void record_max(std::atomic<int>& max, int value)
{
    int prev = max.load();
    while (value > prev && !max.compare_exchange_weak(prev, value)) {}
}

int read_value(ts::Thread_safe<int>& data)
{
    return data.async([](const int& v) { return v; }).get();
}

// --- compile-time constraints ---------------------------------------------

struct Write_fn { void operator()(int&) const {} };
struct Read_fn  { void operator()(const int&) const {} };
struct Int_read { int operator()(const int&) const { return 0; } };
struct Int_write { int operator()(int&) const { return 0; } };

template<typename T, typename Fn>
concept Async_on_mutable = requires(ts::Thread_safe<T>& t, Fn fn) { t.async(fn); };

template<typename T, typename Fn>
concept Async_on_const = requires(const ts::Thread_safe<T>& t, Fn fn) { t.async(fn); };

// Mode deduction routes by parameter const-ness, and write access is unavailable
// through a const handle. These are guarantees, hence static_assert.
static_assert(Async_on_mutable<int, Write_fn>, "T& functor must be writable on a mutable handle");
static_assert(Async_on_mutable<int, Read_fn>, "const T& functor must be callable on a mutable handle");
static_assert(Async_on_const<int, Read_fn>, "const T& functor must be callable through a const handle");
static_assert(!Async_on_const<int, Write_fn>, "T& functor must NOT be callable through a const handle");

static_assert(!std::is_copy_constructible_v<ts::Thread_safe<int>>, "Thread_safe must be non-copyable");
static_assert(!std::is_move_constructible_v<ts::Thread_safe<int>>, "Thread_safe must be non-movable");

// async result type is the functor's return type, on both access paths.
static_assert(std::is_same_v<
    decltype(std::declval<ts::Thread_safe<int>&>().async(std::declval<Int_read>())), ts::Task<int>>);
static_assert(std::is_same_v<
    decltype(std::declval<ts::Thread_safe<int>&>().async(std::declval<Int_write>())), ts::Task<int>>);

void test_type_constraints()
{
    // mirror the static_asserts at runtime so they show in the harness summary
    // (extra parens: the template-argument commas would split the macro args)
    TS_CHECK((Async_on_mutable<int, Write_fn>));
    TS_CHECK((Async_on_mutable<int, Read_fn>));
    TS_CHECK((Async_on_const<int, Read_fn>));
    TS_CHECK((!Async_on_const<int, Write_fn>));
    TS_CHECK(!std::is_copy_constructible_v<ts::Thread_safe<int>>);
    TS_CHECK(!std::is_move_constructible_v<ts::Thread_safe<int>>);
}

// --- pipe / Thread_safe ---------------------------------------------------

void test_serial_correctness()
{
    ts::Thread_safe<Counter> counter;
    for (int i = 0; i < 1000; ++i)
        counter.async([](Counter& c) { c.increment(); });

    int v = counter.async([](const Counter& c) { return c.value(); }).get();
    TS_CHECK(v == 1000);
}

void test_concurrent_readers()
{
    Probe probe;
    ts::Thread_safe<int> data{ 7 };
    std::vector<ts::Task<int>> tasks;

    for (int i = 0; i < 16; ++i)
        tasks.push_back(data.async([&probe](const int& v)
        {
            int now = probe.active.fetch_add(1) + 1;
            record_max(probe.max_readers, now);
            std::this_thread::sleep_for(3ms);
            probe.active.fetch_sub(1);
            return v;
        }));

    for (auto& t : tasks)
        t.get();

    TS_CHECK(probe.max_readers.load() > 1);
}

void test_writer_exclusion()
{
    Probe probe;
    ts::Thread_safe<int> data{ 0 };
    std::vector<ts::Task<void>> tasks;
    int writes = 0;

    for (int round = 0; round < 20; ++round)
    {
        for (int r = 0; r < 4; ++r)
            tasks.push_back(data.async([&probe](const int&)
            {
                probe.active.fetch_add(1);
                if (probe.writing.load())
                    probe.violated.store(true);
                std::this_thread::sleep_for(1ms);
                probe.active.fetch_sub(1);
            }));

        tasks.push_back(data.async([&probe](int& v)
        {
            probe.writing.store(true);
            if (probe.active.fetch_add(1) + 1 != 1)
                probe.violated.store(true);
            std::this_thread::sleep_for(1ms);
            probe.active.fetch_sub(1);
            probe.writing.store(false);
            ++v;
        }));
        ++writes;
    }

    for (auto& t : tasks)
        t.get();

    TS_CHECK(!probe.violated.load());
    TS_CHECK(read_value(data) == writes);
}

// --- Task::then / when_all ------------------------------------------------

void test_continuations()
{
    ts::Thread_safe<int> data{ 21 };

    int chained = data.async([](const int& v) { return v; })
                      .then([](int v) { return v * 2; })
                      .then([](int v) { return v + 1; })
                      .get();
    TS_CHECK(chained == 43);

    std::atomic<int> sink{ 0 };
    data.async([](const int& x) { return x; })
        .then([&sink](int x) { sink.store(x + 100); })
        .get();
    TS_CHECK(sink.load() == 121);
}

void test_when_all()
{
    ts::Thread_safe<int> a{ 10 }, b{ 32 };

    ts::Task<int> ta = a.async([](const int& v) { return v; });
    ts::Task<int> tb = b.async([](const int& v) { return v; });

    int sum = ts::when_all(ta, tb).then([](std::tuple<int, int>& r)
    {
        return std::get<0>(r) + std::get<1>(r);
    }).get();

    TS_CHECK(sum == 42);
}

// --- Static_task_graph ----------------------------------------------------

void test_graph_access_ordering()
{
    ts::Thread_safe<int> a{ 0 }, b{ 0 }, c{ 0 };

    ts::Static_task_graph g;
    g.add_node([](int& x) { x = 1; }, a);
    g.add_node([](const int& x, int& y) { y = x * 10; }, a, b);
    g.add_node([](const int& x, const int& y, int& z) { z = x + y; }, a, b, c);
    g.compile();

    g.execute().get();
    TS_CHECK(read_value(a) == 1);
    TS_CHECK(read_value(b) == 10);
    TS_CHECK(read_value(c) == 11);

    g.execute().get();   // re-run: deterministic
    TS_CHECK(read_value(c) == 11);
}

void test_graph_explicit_ordering()
{
    ts::Thread_safe<int> p{ 0 }, q{ 0 };
    std::atomic<int> seq{ 0 };
    std::atomic<int> p_order{ 0 }, q_order{ 0 };

    ts::Static_task_graph g;
    ts::Task<void> np = g.add_node([&seq, &p_order](int&) { p_order.store(++seq); }, p);
    ts::Task<void> nq = g.add_node([&seq, &q_order](int&) { q_order.store(++seq); }, q);
    nq.after(np);
    g.compile();

    g.execute().get();
    TS_CHECK(p_order.load() == 1);
    TS_CHECK(q_order.load() == 2);
}

// --- death tests (fatal paths run in a subprocess) ------------------------

void test_death_raw_thread_access()
{
    TS_CHECK(ts::test::expect_death("no_context"));
}

void test_death_graph_cycle()
{
    TS_CHECK(ts::test::expect_death("graph_cycle"));
}

void test_death_execute_before_compile()
{
    TS_CHECK(ts::test::expect_death("execute_before_compile"));
}

} // namespace

void run_thread_safe_tests()
{
    std::printf("\n[thread_safe] tests\n");

    run("type constraints", test_type_constraints);
    run("serial correctness", test_serial_correctness);
    run("concurrent readers", test_concurrent_readers);
    run("writer exclusion", test_writer_exclusion);
    run("continuations", test_continuations);
    run("when_all", test_when_all);
    run("graph access ordering", test_graph_access_ordering);
    run("graph explicit ordering", test_graph_explicit_ordering);
    run("death: raw-thread access", test_death_raw_thread_access);
    run("death: graph cycle", test_death_graph_cycle);
    run("death: execute before compile", test_death_execute_before_compile);
}

void run_death_scenario(const char* name)
{
    if (std::strcmp(name, "no_context") == 0)
    {
        Counter c;          // unwrapped; no task context installed
        c.increment();      // -> access violation -> fatal
    }
    else if (std::strcmp(name, "graph_cycle") == 0)
    {
        ts::Thread_safe<int> a{ 0 }, b{ 0 };
        ts::Static_task_graph g;
        ts::Task<void> na = g.add_node([](int&) {}, a);
        ts::Task<void> nb = g.add_node([](int&) {}, b);
        na.after(nb);
        nb.after(na);
        g.compile();        // -> cycle -> fatal
    }
    else if (std::strcmp(name, "execute_before_compile") == 0)
    {
        ts::Static_task_graph g;
        g.execute().get();  // -> fatal
    }
    // unknown scenario: return without dying -> parent's expect_death sees failure
}
