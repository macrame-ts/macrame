#include "guarded_tests.h"
#include "ts/coroutine_support.h"   // the Access_op awaiter tests
#include "ts/guarded.h"
#include "ts/static_task_graph.h"   // the op nested-graph-run gating test
#include "harness.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using ts::test::run;
using ts::test::run_if;
using tests::Counter;
using tests::wait_until;

namespace
{

int read_value(ts::Guarded<int>& data)
{
    return data.async([](const int& v) { return v; }).sync();
}

// --- C: compile-time constraints ------------------------------------------

struct Write_fn { void operator()(int&) const {} };
struct Read_fn  { void operator()(const int&) const {} };
struct Int_read { int operator()(const int&) const { return 0; } };
struct Int_write { int operator()(int&) const { return 0; } };

template<typename T, typename Fn>
concept Async_on_mutable = requires(ts::Guarded<T>& t, Fn fn) { t.async(fn); };

template<typename T, typename Fn>
concept Async_on_const = requires(const ts::Guarded<T>& t, Fn fn) { t.async(fn); };

static_assert(Async_on_mutable<int, Write_fn>);
static_assert(Async_on_mutable<int, Read_fn>);
static_assert(Async_on_const<int, Read_fn>);
static_assert(!Async_on_const<int, Write_fn>, "write must not be callable through a const handle");
static_assert(!std::is_copy_constructible_v<ts::Guarded<int>>);
static_assert(!std::is_move_constructible_v<ts::Guarded<int>>);

// Generic (probed) classification: `auto&` = write (an rvalue cannot bind it), so it must not
// be callable through a const handle; `const auto&` = read, so it must be.
using Generic_write = decltype([](auto& v) { ++v; });
using Generic_read = decltype([](const auto& v) { (void)v; });
static_assert(Async_on_mutable<int, Generic_write>);
static_assert(!Async_on_const<int, Generic_write>, "generic write probes read_write");
static_assert(Async_on_const<int, Generic_read>, "generic const read probes read_only");

#if 0   // Compile-time rejections, kept for documentation - each of these must not compile:
void must_not_compile(ts::Guarded<int>& g)
{
    g.async([](int v) { ++v; });            // by-value resource param: static_assert (a copy
                                            // would silently discard the writes)
    g.async([](int&& v) { (void)v; });      // rvalue-ref resource param: static_assert
    g.async([](const auto& v) { ++v; });    // mutating body under a read classification:
                                            // read bodies receive `const T&`
}
void must_not_compile_generic_by_value(ts::Guarded<tests::Counter>& g)
{
    g.async([](auto v) { (void)v; });       // generic by-value param on a class type: the copy
                                            // probe's static_assert (silent-copy rejection)
}
#endif

// The copy probe's boundaries. A generic read on a class type still probes read (the probe
// instantiates the body with a `Non_copyable<Counter>&`, which resolves through the base).
// For a final class no stand-in can be derived, so the check is skipped and a generic
// by-value parameter stays the documented silent-copy residual - it must keep compiling.
static_assert(Async_on_const<tests::Counter, Generic_read>, "copy probe passes a real read body");
struct Final_counter final { int n = 0; };
using Generic_by_value = decltype([](auto v) { (void)v; });
static_assert(Async_on_const<Final_counter, Generic_by_value>, "final type: copy probe skipped, residual documented");
static_assert(std::is_same_v<
    decltype(std::declval<ts::Guarded<int>&>().async(std::declval<Int_read>())), ts::Task<int>>);
static_assert(std::is_same_v<
    decltype(std::declval<ts::Guarded<int>&>().async(std::declval<Int_write>())), ts::Task<int>>);

void test_type_constraints()
{
    TS_CHECK((Async_on_mutable<int, Write_fn>));
    TS_CHECK((Async_on_const<int, Read_fn>));
    TS_CHECK((!Async_on_const<int, Write_fn>));
    TS_CHECK(!std::is_copy_constructible_v<ts::Guarded<int>>);
    TS_CHECK(!std::is_move_constructible_v<ts::Guarded<int>>);
}

// --- C: basics ------------------------------------------------------------

void test_construct()
{
    ts::Guarded<int> d{ ts::Named{}, 5 };
    TS_CHECK(read_value(d) == 5);
}

void test_write_then_read()
{
    ts::Guarded<Counter> c{ ts::Named{} };
    c.async([](Counter& x) { x.add(7); });
    int v = c.async([](const Counter& x) { return x.value(); }).sync();
    TS_CHECK(v == 7);
}

void test_async_returns_value()
{
    ts::Guarded<int> d{ ts::Named{}, 41 };
    ts::Task<int> t = d.async([](const int& v) { return v + 1; });
    TS_CHECK(t.sync() == 42);
}

void test_destructor_waits()
{
    constexpr int count = 1000;
    std::atomic<int> done{ 0 };
    {
        ts::Guarded<int> d{ ts::Named{}, 0 };
        for (int i = 0; i < count; ++i)
            d.async([&done](int& v) { ++v; done.fetch_add(1); });
    }   // destructor waits for the pipe to drain
    TS_CHECK(done.load() == count);
}

// A task settles in the order settle -> notify waiters -> `on_complete` -> pipe release
// (`Task_control_block::settle`), so `sync()` returns while the settling thread still holds
// the object's pipe grant: the release trails the waiter's wake. Destroying the object right
// there is nevertheless defined, because `~Guarded` drains through `Pipe::wait_until_idle` --
// the destructor cannot pass the drain until `release_and_redispatch` has cleared the grant,
// and that notify happens under `Pipe::mutex`, so the signaler is done with the pipe before
// the waiter can reacquire it and return. This loop is the regression guard: the objects are
// heap-allocated, so a release that outlived its destructor surfaces as a use-after-free
// under ASan/TSan instead of as a rare hang. The concurrent threads keep the pool busy, which
// is what makes the settling worker likely to be preempted inside the window.
void test_sync_then_destroy()
{
    constexpr int threads = 4;
    constexpr int iterations = 2000;
    std::atomic<int> mismatches{ 0 };
    {
        std::vector<std::jthread> racers;
        for (int t = 0; t < threads; ++t)
        {
            racers.emplace_back([&]
            {
                for (int i = 0; i < iterations; ++i)
                {
                    auto* data = new ts::Guarded<int>{ ts::Named{}, 0 };
                    data->async([](int& v) { ++v; });   // fire-and-forget, queued ahead of the target
                    // Alternate the mode of the last access: a writer release clears
                    // `writer_active`, a reader release decrements `active_readers`, and both
                    // run the drain notify.
                    int seen = i % 2 == 0
                        ? data->async([](const int& v) { return v; }).sync()
                        : data->async([](int& v) { v *= 10; return v; }).sync();
                    delete data;   // immediately after the wake, while release may still be pending
                    if (seen != (i % 2 == 0 ? 1 : 10))
                        mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    }   // join
    TS_CHECK(mismatches.load() == 0);
}

// The multi-object shape of the same window: `advance_pipe_links` releases the block's links
// in order, so the first object's destructor can run while the settling thread is still
// walking the rest of the array. Each pipe is drained by its own destructor, and the link
// array belongs to the block (kept alive by the running frame's reference), not to any pipe.
void test_multi_sync_then_destroy()
{
    constexpr int iterations = 2000;
    int mismatches = 0;
    for (int i = 0; i < iterations; ++i)
    {
        auto* first = new ts::Guarded<int>{ ts::Named{}, 1 };
        auto* second = new ts::Guarded<int>{ ts::Named{}, 2 };
        if (ts::async([](int& x, const int& y) { x += y; return x; }, *first, *second).sync() != 3)
            ++mismatches;
        // Both deletion orders, so whichever pipe the cascade released first is covered.
        if (i % 2 == 0)
        {
            delete first;
            delete second;
        }
        else
        {
            delete second;
            delete first;
        }
    }
    TS_CHECK(mismatches == 0);
}

// --- D: reader/writer pipe ------------------------------------------------

void test_serial_correctness()
{
    ts::Guarded<Counter> counter{ ts::Named{} };
    for (int i = 0; i < 1000; ++i)
        counter.async([](Counter& c) { c.increment(); });
    int v = counter.async([](const Counter& c) { return c.value(); }).sync();
    TS_CHECK(v == 1000);
}

void test_concurrent_readers()
{
    // Deterministic concurrency check: two readers each wait (bounded) for the other, so
    // the gate is met iff the pipe genuinely ran readers concurrently - rather than the
    // old "peak > 1" that merely hoped the timing overlapped.
    tests::Parallel_gate gate{ 2 };
    ts::Guarded<int> data{ ts::Named{}, 7 };
    std::vector<ts::Task<int>> tasks;

    for (int i = 0; i < 16; ++i)
    {
        tasks.push_back(data.async([&gate](const int& v)
        {
            gate.arrive();
            return v;
        }));
    }

    for (auto& t : tasks)
        t.sync();
    TS_CHECK(gate.met());   // concurrent readers (not serialized by the reader/writer pipe)
}

void test_writer_exclusion()
{
    std::atomic<int> active{ 0 };
    std::atomic<bool> writing{ false }, violated{ false };
    ts::Guarded<int> data{ ts::Named{}, 0 };
    std::vector<ts::Task<void>> tasks;
    int writes = 0;

    for (int round = 0; round < 20; ++round)
    {
        for (int r = 0; r < 4; ++r)
        {
            tasks.push_back(data.async([&active, &writing, &violated](const int&)
            {
                active.fetch_add(1);
                if (writing.load())
                    violated.store(true);
                std::this_thread::sleep_for(1ms);
                active.fetch_sub(1);
            }));
        }

        tasks.push_back(data.async([&active, &writing, &violated](int& v)
        {
            writing.store(true);
            if (active.fetch_add(1) + 1 != 1)
                violated.store(true);
            std::this_thread::sleep_for(1ms);
            active.fetch_sub(1);
            writing.store(false);
            ++v;
        }));
        ++writes;
    }

    for (auto& t : tasks)
        t.sync();
    TS_CHECK(!violated.load());
    TS_CHECK(read_value(data) == writes);
}

void test_reader_after_writer()
{
    ts::Guarded<int> data{ ts::Named{}, 0 };
    data.async([](int& v) { v = 99; });
    TS_CHECK(read_value(data) == 99);   // FIFO: the read sees the prior write
}

void test_independent_objects_parallel()
{
    tests::Parallel_gate gate{ 2 };
    ts::Guarded<int> a{ ts::Named{}, 0 }, b{ ts::Named{}, 0 };

    auto job = [&gate](int&) { gate.arrive(); };
    ts::Task<void> ta = a.async(job);
    ts::Task<void> tb = b.async(job);
    ta.sync();
    tb.sync();
    TS_CHECK(gate.met());   // separate pipes ran concurrently
}

// --- D: generic (probed) accessors ----------------------------------------
// No tags: a generic `[](const auto&)` probes read_only (it binds an rvalue), `[](auto&)`
// probes read_write (it cannot). Same admission behavior as the spelled-out forms.

void test_generic_readers_overlap()
{
    tests::Parallel_gate gate{ 2 };
    ts::Guarded<int> data{ ts::Named{}, 7 };
    std::vector<ts::Task<int>> tasks;

    for (int i = 0; i < 8; ++i)
    {
        tasks.push_back(data.async([&gate](const auto& v)
        {
            gate.arrive();
            return v;
        }));
    }

    for (auto& t : tasks)
        t.sync();
    TS_CHECK(gate.met());   // probed read_only: readers ran concurrently
}

void test_generic_writer_serializes()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };
    for (int i = 0; i < 200; ++i)
        d.async([](auto& v) { ++v; });   // probed read_write: exclusive, all land
    TS_CHECK(read_value(d) == 200);
}

void test_reentrant_same_object()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };
    std::atomic<int> done{ 0 };

    d.async([&d, &done](int& v)
    {
        v += 1;
        d.async([&done](int& w) { w += 1; done.fetch_add(1); });   // same pipe
        done.fetch_add(1);
    });

    wait_until([&] { return done.load() == 2; });
    TS_CHECK(read_value(d) == 2);
}

void test_reentrant_other_object()
{
    ts::Guarded<int> a{ ts::Named{}, 0 }, b{ ts::Named{}, 0 };
    std::atomic<int> done{ 0 };

    a.async([&b, &done](int& v)
    {
        v = 1;
        b.async([&done](int& w) { w = 1; done.fetch_add(1); });
        done.fetch_add(1);
    });

    wait_until([&] { return done.load() == 2; });
    TS_CHECK(read_value(a) == 1 && read_value(b) == 1);
}

void test_pipe_stress()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };
    for (int i = 0; i < 5000; ++i)
        d.async([](int& v) { ++v; });
    TS_CHECK(read_value(d) == 5000);
}

// --- access (inline-when-free) vs async (always enqueued) ----------------

// A write `access` on a free pipe runs synchronously on the calling thread; the returned task
// is already settled when the call returns.
void test_access_runs_synchronously()
{
    ts::Guarded<int> d{ ts::Named{}, 5 };
    std::thread::id body_thread{};
    auto t = d.access([&body_thread](int& v)
    {
        body_thread = std::this_thread::get_id();
        return ++v;   // 6
    });

    TS_CHECK(t.is_done());                                  // ran before this line returned
    TS_CHECK(body_thread == std::this_thread::get_id());    // on the calling thread
    TS_CHECK(t.sync() == 6);
    TS_CHECK(read_value(d) == 6);
}

// A read `access` on a free pipe joins as a reader and runs on the caller.
void test_access_read_on_caller()
{
    ts::Guarded<int> d{ ts::Named{}, 9 };
    std::thread::id body_thread{};
    int r = d.access([&body_thread](const int& v)
    {
        body_thread = std::this_thread::get_id();
        return v;
    }).sync();
    TS_CHECK(r == 9);
    TS_CHECK(body_thread == std::this_thread::get_id());
}

// When the pipe is busy (a writer holds it), `access` cannot acquire it, so it defers to the
// queue and runs correctly on a worker once the pipe drains.
void test_access_falls_back_when_busy()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };
    std::atomic<bool> gate{ false };
    std::thread::id caller = std::this_thread::get_id();
    std::atomic<std::thread::id> body_thread{};

    // Occupy the pipe with a writer that holds it until `gate` (writer_active is set
    // synchronously by dispatch, so the pipe is busy by the time this call returns).
    ts::Task<void> blocker = d.async([&gate](int&)
    {
        while (!gate.load()) std::this_thread::yield();
    });

    // `access` while the writer holds the pipe -> must defer (not run on the caller).
    auto t = d.access([&body_thread](int& v)
    {
        body_thread.store(std::this_thread::get_id());
        return ++v;
    });

    TS_CHECK(!t.is_done());          // deferred behind the blocker, not run inline

    gate.store(true);
    TS_CHECK(t.sync() == 1);          // ran correctly after the blocker drained
    TS_CHECK(body_thread.load() != caller);   // on a worker, not the calling thread
    blocker.sync();
}

// `async` always enqueues - even on a free pipe it does not run on the calling thread.
// Asserted via the executing thread id: an inline regression would run the body on the
// caller (synchronously, before `async` returns), failing the id check deterministically.
// A `!t.is_done()` probe after `async` returns is not sound - a fast worker can complete
// the body before the check runs (it cost a CI flake on a slow runner).
void test_async_always_schedules()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };
    std::thread::id caller = std::this_thread::get_id();
    std::atomic<std::thread::id> body_thread{ caller };
    ts::Task<int> t = d.async([&body_thread](int& v)
    {
        body_thread.store(std::this_thread::get_id());
        return ++v;
    });
    TS_CHECK(t.sync() == 1);
    TS_CHECK(body_thread.load() != caller);    // on a worker, never inline on the caller
    TS_CHECK(read_value(d) == 1);
}

// A read `async` also always enqueues and still deduces read (concurrent) access.
void test_async_read_schedules()
{
    ts::Guarded<int> d{ ts::Named{}, 7 };
    TS_CHECK(d.async([](const int& v) { return v; }).sync() == 7);
}

// --- B: FIFO group ordering (pipe-rebase regression guard) ----------------

// B2: arrival R1, W, R2 - the writer between the two reads closes R1's group, so R2 forms
// a new group after the writer. A shared sequence counter stamps execution order; R2 must
// run after W (never join R1's group) and observe the write.
void test_rwr_group_separation()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };
    std::atomic<int> seq{ 0 };
    std::atomic<int> r1_at{ -1 }, w_at{ -1 }, r2_at{ -1 };

    ts::Task<int> r1 = d.async([&](const int& v) { r1_at = seq.fetch_add(1); return v; });
    ts::Task<void> w = d.async([&](int& v) { w_at = seq.fetch_add(1); v = 5; });
    ts::Task<int> r2 = d.async([&](const int& v) { r2_at = seq.fetch_add(1); return v; });

    int r1v = r1.sync();
    w.sync();
    int r2v = r2.sync();

    TS_CHECK(r1_at.load() < w_at.load() && w_at.load() < r2_at.load());   // R1 < W < R2
    TS_CHECK(r1v == 0);   // R1 ran before the write
    TS_CHECK(r2v == 5);   // R2 ran after it -> did not join R1's group
}

// B4: writers run in launch order - an order-sensitive fold composes to one exact value.
void test_writer_fifo()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };
    for (int i = 1; i <= 6; ++i)
        d.async([i](int& v) { v = v * 10 + i; });
    TS_CHECK(read_value(d) == 123456);   // FIFO: (((((1)*10+2)*10+3)...)
}

// --- Access_op: the caller-owned operation state `access` returns ---------

// Pinned by contract: the pipe's intrusive FIFO holds the embedded entry's address.
static_assert(!std::is_copy_constructible_v<ts::Access_op<int, Int_read>>);
static_assert(!std::is_move_constructible_v<ts::Access_op<int, Int_read>>);
static_assert(!std::is_copy_assignable_v<ts::Access_op<int, Int_read>>);
static_assert(!std::is_move_assignable_v<ts::Access_op<int, Int_read>>);
// Mode and result deduce from the body, one spelling rule.
static_assert(ts::Access_op<int, Int_read>::mode == ts::Access::read_only);
static_assert(ts::Access_op<int, Int_write>::mode == ts::Access::read_write);
static_assert(std::is_same_v<ts::Access_op<int, Write_fn>::result_type, void>);
// The single-object spelling still names one object: the arity split is objects-then-body, so
// `Access_op<T, Body>` reads the same as it always did.
static_assert(ts::Access_op<int, Int_read>::arity == 1);

// The same type at higher arity - objects first, body last - with a mode per position.
struct Two_add { int operator()(int&, const int&) const { return 0; } };
static_assert(ts::Access_op<int, int, Two_add>::arity == 2);
static_assert(ts::Access_op<int, int, Two_add>::mode_at<0> == ts::Access::read_write);
static_assert(ts::Access_op<int, int, Two_add>::mode_at<1> == ts::Access::read_only);
static_assert(std::is_same_v<ts::Access_op<int, int, Two_add>::result_type, int>);
static_assert(!std::is_move_constructible_v<ts::Access_op<int, int, Two_add>>);

// A void access: `sync()` returns nothing, the write lands.
void test_op_void_result()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };
    d.access([](int& v) { v = 42; }).sync();
    TS_CHECK(read_value(d) == 42);
}

// The member spelling the flattened Body exists for (docs/internals/access-op-design.md §10 tier 1): a
// named functor as `Body`, the op a directly-declared member, constructed in place by
// guaranteed elision through the `access` return.
void test_op_member_storage()
{
    struct Snapshot
    {
        int bonus;
        int operator()(const int& v) const { return v + bonus; }
    };
    struct Holder
    {
        ts::Access_op<int, Snapshot> op;
        explicit Holder(ts::Guarded<int>& g) : op(g.access(Snapshot{ 3 })) {}
    };

    ts::Guarded<int> d{ ts::Named{}, 7 };
    Holder h(d);
    TS_CHECK(h.op.is_done());   // free pipe: ran inline during Holder's construction
    TS_CHECK(h.op.sync() == 10);
}

// `try_take`: empty while unsettled, the moved value once settled, empty when cancelled.
void test_op_try_take()
{
    ts::Guarded<int> d{ ts::Named{}, 5 };
    std::atomic<bool> gate{ false };
    ts::Task<void> blocker = d.async([&gate](int&) { while (!gate.load()) std::this_thread::yield(); });

    auto op = d.access([](const int& v) { return v; });
    TS_CHECK(!op.try_take().has_value());   // queued behind the blocker: unsettled
    gate.store(true);
    blocker.sync();
    TS_CHECK(op.sync() == 5);
}

// `co_await op.as_optional()` completes the vocabulary `Task` already had: the value on a
// completed access, empty on a cancelled one instead of the fatal a bare `co_await` raises.
ts::Task<int> await_op_as_optional(ts::Guarded<int>& d, ts::Cancellation_token token)
{
    auto op = d.access([](const int& v) { return v; }, { .token = token });
    std::optional<int> got = co_await op.as_optional();
    co_return got.value_or(-1);
}

void test_op_as_optional()
{
    ts::Guarded<int> d{ ts::Named{}, 5 };
    TS_CHECK(await_op_as_optional(d, {}).sync() == 5);

    ts::Cancellation_source src;
    src.request_cancel();
    TS_CHECK(await_op_as_optional(d, src.token()).sync() == -1);   // cancelled: empty, no fatal
}

// Cancellation mirrors `Task`: a pre-cancelled token skips the body, the op settles
// cancelled, `try_take` is empty, a void `sync()` returns normally.
void test_op_cancellation()
{
    ts::Guarded<int> d{ ts::Named{}, 1 };
    ts::Cancellation_source src;
    src.request_cancel();

    auto value_op = d.access([](const int& v) { return v; }, { .token = src.token() });
    TS_CHECK(value_op.is_done());
    TS_CHECK(value_op.is_cancelled());
    TS_CHECK(!value_op.try_take().has_value());

    auto void_op = d.access([](int& v) { v = 99; }, { .token = src.token() });
    TS_CHECK(void_op.is_cancelled());
    void_op.sync();   // a cancelled void access unblocks normally
    TS_CHECK(read_value(d) == 1);   // the body never ran

    // Queued then cancelled-before-dispatch: the turn is taken and released, the op settles
    // cancelled, and the pipe drains normally behind it.
    std::atomic<bool> gate{ false };
    ts::Task<void> blocker = d.async([&gate](int&) { while (!gate.load()) std::this_thread::yield(); });
    auto queued = d.access([](const int& v) { return v; }, { .token = src.token() });
    TS_CHECK(!queued.is_done());
    gate.store(true);
    blocker.sync();
    TS_CHECK(!queued.try_take().has_value());
    while (!queued.is_done())
        std::this_thread::yield();
    TS_CHECK(queued.is_cancelled());
}

// `co_await obj.access(fn)` - the frame-resident temporary, across a genuine suspension: the
// pipe is held when the coroutine starts, so the awaiter suspends and the settling worker
// resumes the frame (destroying the op inside the resume - the custody shape the
// caller-owned settle exists for).
void test_op_await_suspending()
{
    ts::Guarded<int> d{ ts::Named{}, 11 };
    std::atomic<bool> gate{ false };
    ts::Task<void> blocker = d.async([&gate](int&) { while (!gate.load()) std::this_thread::yield(); });

    auto frame = [&]() -> ts::Task<int>
    {
        int v = co_await d.access([](const int& x) { return x; });
        co_return v + 1;
    }();
    TS_CHECK(!frame.is_done());   // suspended behind the blocker
    gate.store(true);
    blocker.sync();
    TS_CHECK(frame.sync() == 12);
}

// The named-op form: eager start at the declaration, awaited later (overlap for free); the
// settled await never suspends.
void test_op_await_named()
{
    ts::Guarded<int> d{ ts::Named{}, 20 };
    auto frame = [&]() -> ts::Task<int>
    {
        auto op = d.access([](const int& x) { return x * 2; });   // free pipe: settles in the ctor
        co_return co_await op;
    }();
    TS_CHECK(frame.sync() == 40);
}

// Destroying a started-but-unsettled op: checked builds report it (`TS_ENSURE`), every build
// then blocks until the access settles - the write must have landed by the time the
// destructor returns.
void test_op_dtor_waits_unsettled()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };
    std::atomic<bool> gate{ false };
    ts::Task<void> blocker = d.async([&gate](int&) { while (!gate.load()) std::this_thread::yield(); });
    std::thread releaser;
    {
#if TS_SAFETY_CHECKS
        ts::test::Expected_ensures expected(1);
#endif
        auto op = d.access([](int& v) { v = 7; });
        TS_CHECK(!op.is_done());
        releaser = std::thread([&gate] { std::this_thread::sleep_for(10ms); gate.store(true); });
    }   // ~Access_op: ensure fires (checked), then blocks until the write settles
    releaser.join();
    blocker.sync();
    TS_CHECK(read_value(d) == 7);   // the dtor waited the access out; nothing was lost
}

// --- Multi-object Access_op ------------------------------------------------

// `ts::access(fn, a, b)` returns the same caller-owned op the single-object verb does: one
// consume vocabulary at every arity, the body invoked in DECLARATION order however the pipes
// happened to sort, and `start()` refiring the same storage.
void test_multi_op_basic()
{
    ts::Guarded<int> a{ ts::Named{}, 1 };
    ts::Guarded<int> b{ ts::Named{}, 2 };
    auto op = ts::access([](int& x, const int& y) { x += y; return x; }, a, b);
    TS_CHECK(op.sync() == 3);
    op.start();
    TS_CHECK(op.sync() == 5);
    TS_CHECK(read_value(a) == 5);
    TS_CHECK(read_value(b) == 2);
}

// Declaration order is independent of canonical (pipe-address) order: the same body over the
// same pair, declared both ways round, writes the object it was given first.
void test_multi_op_declaration_order()
{
    ts::Guarded<int> a{ ts::Named{}, 10 };
    ts::Guarded<int> b{ ts::Named{}, 3 };
    TS_CHECK(ts::access([](int& x, const int& y) { x -= y; return x; }, a, b).sync() == 7);
    TS_CHECK(ts::access([](int& x, const int& y) { x -= y; return x; }, b, a).sync() == -4);
}

// Awaited from a coroutine, across a real suspension: the op is frame-resident, and the
// settling worker resumes the frame.
void test_multi_op_await()
{
    ts::Guarded<int> a{ ts::Named{}, 4 };
    ts::Guarded<int> b{ ts::Named{}, 5 };
    std::atomic<bool> gate{ false };
    ts::Task<void> blocker = b.async([&gate](int&) { while (!gate.load()) std::this_thread::yield(); });

    auto frame = [&]() -> ts::Task<int>
    {
        co_return co_await ts::access([](const int& x, const int& y) { return x * y; }, a, b);
    }();
    TS_CHECK(!frame.is_done());   // suspended behind the blocker on b
    gate.store(true);
    blocker.sync();
    TS_CHECK(frame.sync() == 20);
}

// Every object free: the whole set is admitted in one pass and the body runs on the CALLING
// thread, like the single-object inline arm. The thread-id check is what keeps the rest of
// this group honest - without it "ran inline" could pass vacuously on a queued dispatch.
void test_multi_op_inline_when_all_free()
{
    ts::Guarded<int> a{ ts::Named{}, 1 };
    ts::Guarded<int> b{ ts::Named{}, 2 };
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<std::thread::id> body_thread{};
    auto op = ts::access([&body_thread](int& x, const int& y)
    {
        body_thread.store(std::this_thread::get_id());
        x += y;
        return x;
    }, a, b);
    TS_CHECK(op.is_done());                     // admitted and run before the verb returned
    TS_CHECK(body_thread.load() == caller);
    TS_CHECK(op.sync() == 3);
}

// One object busy defers the WHOLE op: the probe admits nothing and the body runs off the
// calling thread once the busy object drains. (A failed probe leaves no trace to observe
// afterwards - what follows it is the queued cascade, which legitimately holds each object it
// has reached while it waits for the next, in canonical order.)
void test_multi_op_defers_when_one_busy()
{
    ts::Guarded<int> a{ ts::Named{}, 1 };
    ts::Guarded<int> b{ ts::Named{}, 2 };
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> gate{ false };
    std::atomic<bool> holding{ false };
    ts::Task<void> blocker = b.async([&gate, &holding](int&)
    {
        holding.store(true);
        while (!gate.load())
            std::this_thread::yield();
    });
    wait_until([&holding] { return holding.load(); });
    // Armed before the probe below, so a partial-admission regression fails the check instead
    // of hanging the suite.
    std::thread releaser([&gate] { std::this_thread::sleep_for(200ms); gate.store(true); });

    std::atomic<std::thread::id> body_thread{};
    auto op = ts::access([&body_thread](int& x, const int& y)
    {
        body_thread.store(std::this_thread::get_id());
        x += y;
        return x;
    }, a, b);
    TS_CHECK(!op.is_done());   // b is held: the probe failed and the op queued

    releaser.join();
    blocker.sync();
    TS_CHECK(op.sync() == 3);
    TS_CHECK(body_thread.load() != caller);
}

// Fairness per pipe: an entry QUEUED on any one object suppresses the inline arm even when
// that object's mode rules would admit the op right now. Here `b` has a live reader (so a
// second read is admissible) with a writer queued behind it - the op reads `b`, and taking it
// inline would jump that writer.
void test_multi_op_never_jumps_queued_entry()
{
    ts::Guarded<int> a{ ts::Named{}, 0 };
    ts::Guarded<int> b{ ts::Named{}, 1 };
    std::atomic<bool> gate{ false };
    std::atomic<bool> reading{ false };
    ts::Task<void> reader = b.async([&gate, &reading](const int&)
    {
        reading.store(true);
        while (!gate.load())
            std::this_thread::yield();
    });
    wait_until([&reading] { return reading.load(); });
    ts::Task<void> writer = b.async([](int& v) { v = 99; });   // queued behind the live reader
    std::thread releaser([&gate] { std::this_thread::sleep_for(200ms); gate.store(true); });

    auto op = ts::access([](int& x, const int& y) { x = y; return x; }, a, b);
    TS_CHECK(!op.is_done());   // admissible on b, but queued work is never jumped

    releaser.join();
    reader.sync();
    writer.sync();
    TS_CHECK(op.sync() == 99);   // ran after the queued writer: FIFO held
}

// Two multi-object READ ops over the same pair overlap - the inline arm joins both as readers
// on both pipes rather than serializing them.
void test_multi_op_readers_overlap()
{
    ts::Guarded<int> a{ ts::Named{}, 1 };
    ts::Guarded<int> b{ ts::Named{}, 2 };
    tests::Parallel_gate gate(2);
    auto body = [&gate](const int& x, const int& y)
    {
        gate.arrive();
        return x + y;
    };
    // `TS_CHECK` bumps the harness's plain counters, so it is called from the test thread
    // only: each worker hands its result back and the checks run after the joins.
    int first_sum = 0;
    int second_sum = 0;
    std::thread first([&] { first_sum = ts::access(body, a, b).sync(); });
    std::thread second([&] { second_sum = ts::access(body, a, b).sync(); });
    first.join();
    second.join();
    TS_CHECK(first_sum == 3);
    TS_CHECK(second_sum == 3);
    TS_CHECK(gate.met());   // both bodies were in flight at once
}

// The canonical order is global, so the DECLARED order cannot deadlock: two threads hammer the
// same pair of objects, each writing both, declared opposite ways round.
void test_multi_op_opposite_orders_stress()
{
    constexpr int iterations = 3000;
    ts::Guarded<int> a{ ts::Named{ "a" }, 0 };
    ts::Guarded<int> b{ ts::Named{ "b" }, 0 };
    auto bump = [](int& x, int& y) { ++x; ++y; };
    std::thread forward([&] { for (int i = 0; i < iterations; ++i) ts::access(bump, a, b).sync(); });
    std::thread backward([&] { for (int i = 0; i < iterations; ++i) ts::access(bump, b, a).sync(); });
    forward.join();
    backward.join();
    TS_CHECK(read_value(a) == 2 * iterations);
    TS_CHECK(read_value(b) == 2 * iterations);
}

// The lend protocol, partial: the calling task already holds `a`, so the op takes a turn on
// `b` alone and runs. Queueing on `a` instead would put the op behind its own caller's hold -
// the deadlock lending exists to remove - so this test cannot pass by accident.
void test_multi_op_lend_partial()
{
    ts::Guarded<int> a{ ts::Named{ "a" }, 1 };
    ts::Guarded<int> b{ ts::Named{ "b" }, 2 };
    std::atomic<int> got{ -1 };
    a.async([&a, &b, &got](int& x)
    {
        x = 10;
        auto op = ts::access([](int& p, const int& q) { p += q; return p; }, a, b);
        got.store(op.try_take().value_or(-1));   // `b` was free too, so the op settled inline
    }).sync();
    TS_CHECK(got.load() == 12);
    TS_CHECK(read_value(a) == 12);
}

// Every object lent: nothing to acquire, so the body runs inline on the holder's thread under
// the grants it already has - what the single-object reentrant arm did, generalized.
void test_multi_op_lend_all()
{
    ts::Guarded<int> a{ ts::Named{ "a" }, 1 };
    ts::Guarded<int> b{ ts::Named{ "b" }, 2 };
    std::atomic<std::thread::id> holder{};
    std::atomic<std::thread::id> lent_body{};
    std::atomic<int> got{ -1 };
    ts::async([&a, &b, &holder, &lent_body, &got](int& x, int& y)
    {
        holder.store(std::this_thread::get_id());
        (void)x;
        (void)y;
        auto op = ts::access([&lent_body](int& p, const int& q)
        {
            lent_body.store(std::this_thread::get_id());
            p += q;
            return p;
        }, a, b);
        got.store(op.try_take().value_or(-1));
    }, a, b).sync();
    TS_CHECK(got.load() == 3);
    TS_CHECK(lent_body.load() == holder.load());
    TS_CHECK(read_value(a) == 3);
}

// --- Access_op lifecycle: construct / bind / start (§10.1) -----------------

struct Read_plus
{
    int add = 0;
    int operator()(const int& v) const { return v + add; }
};

// Unbound and dormant ops destroy without any check firing (a dormant body is a capability,
// not a pending effect); bind-then-start is the deferred spelling of the eager constructor.
void test_op_lifecycle_bind_start()
{
    ts::Guarded<int> d{ ts::Named{}, 4 };
    {
        ts::Access_op<int, Read_plus> unbound;   // trivial destroy
    }
    {
        ts::Access_op<int, Read_plus> parked(ts::dormant, d, Read_plus{ 1 });   // body destroyed, no check
        TS_CHECK(!parked.is_done());
    }
    ts::Access_op<int, Read_plus> op;
    op.bind(d, Read_plus{ 10 });
    TS_CHECK(!op.is_done());   // bound, not fired
    op.start();
    TS_CHECK(op.sync() == 14);
}

// start() refires a settled op from the same storage: fresh values each cycle, the consumed
// flag re-arms, and an unconsumed result is discarded by the next start() (the documented
// skip-a-stale-frame steady state).
void test_op_refire()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };
    auto op = d.access(Read_plus{});
    TS_CHECK(op.sync() == 0);
    for (int frame = 1; frame <= 5; ++frame)
    {
        d.async([](int& v) { ++v; }).sync();
        op.start();
        TS_CHECK(op.take() == frame);   // consuming each cycle; start() re-arms the flag
    }
    op.start();   // settle, never consume...
    while (!op.is_done())
        std::this_thread::yield();
    op.start();   // ...then refire over the unconsumed result: discarded, legal
    TS_CHECK(op.sync() == 5);
}

// Rebind on settled retargets the same op storage to a DIFFERENT Guarded (pipe refs are
// drained at settle, so the embedded entry is free) - the pooled/reused-op enabler.
void test_op_rebind_settled()
{
    ts::Guarded<int> a{ ts::Named{}, 100 };
    ts::Guarded<int> b{ ts::Named{}, 200 };
    ts::Access_op<int, Read_plus> op(ts::dormant, a, Read_plus{ 1 });
    op.start();
    TS_CHECK(op.sync() == 101);
    op.bind(b, Read_plus{ 2 });   // settled: rebind destroys the old body, retargets
    op.start();
    TS_CHECK(op.sync() == 202);
}

// The consume/peek vocabulary (§5 revised): `sync() &` is a repeatable non-consuming peek,
// `take()` the explicit consuming move, `sync() &&` the temporary form's by-value consume.
void test_op_sync_take_vocabulary()
{
    ts::Guarded<int> d{ ts::Named{}, 7 };
    auto op = d.access([](const int& v) { return v; });
    TS_CHECK(op.sync() == 7);
    TS_CHECK(op.sync() == 7);                                    // sync() & does not consume
    TS_CHECK(op.take() == 7);                                    // peek then take: legal, take moves
    TS_CHECK(d.access([](const int& v) { return v; }).sync() == 7);   // temporary: by value
}

// `Access_options{.queued = true}`: attended but never-inline - the body runs off the
// calling thread even on a free pipe. A lent object ignores the option (queuing behind
// the caller's own held grant would deadlock when awaited).
void test_op_queued_option()
{
    ts::Guarded<int> d{ ts::Named{}, 3 };
    std::thread::id caller = std::this_thread::get_id();
    std::atomic<std::thread::id> body_thread{};
    auto op = d.access([&body_thread](const int& v)
    {
        body_thread.store(std::this_thread::get_id());
        return v;
    }, { .queued = true });
    TS_CHECK(op.sync() == 3);
    TS_CHECK(body_thread.load() != caller);   // free pipe, still dispatched to a worker

    // Reentrant + queued: runs inline under the held grant regardless of the option.
    std::atomic<bool> inner_ok{ false };
    d.async([&d, &inner_ok](int& v)
    {
        v = 9;
        auto inner = d.access([](const int& x) { return x; }, { .queued = true });
        inner_ok.store(inner.is_done() && inner.try_take().value_or(-1) == 9);
    }).sync();
    TS_CHECK(inner_ok.load());
}

// `Access_options::priority` / `Dispatch_options::priority` unset = inherit: an `async` issued
// from inside a launched task dispatches at the launcher's priority (`detail::resolved_priority`,
// the rule `parallel_for` already follows), read off the body's own block; an explicit option
// still wins; outside a task it is `normal`.
void test_async_inherits_caller_priority()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };
    auto own_priority = [](int&) { return static_cast<int>(ts::detail::Current_task::get()->flags.priority); };

    TS_CHECK(d.async(own_priority).sync() == static_cast<int>(ts::Priority::normal));   // outside a task

    // The launched body only issues the async (it cannot sync inside a task); the handle is
    // joined here, on the test thread.
    const ts::Priority classes[] = { ts::Priority::high, ts::Priority::normal, ts::Priority::low };
    for (ts::Priority p : classes)
    {
        ts::Task<int> probe;
        ts::launch([&d, &probe, own_priority] { probe = d.async(own_priority); }, { .priority = p }).sync();
        TS_CHECK(probe.sync() == static_cast<int>(p));
    }

    ts::Task<int> forced;
    ts::launch([&d, &forced, own_priority] { forced = d.async(own_priority, { .priority = ts::Priority::low }); },
        { .priority = ts::Priority::high }).sync();
    TS_CHECK(forced.sync() == static_cast<int>(ts::Priority::low));
}

// --- Access_op: nested completion-gating (s4 as revised) -------------------

// A functor op body that starts a nested graph run and RETURNS: `execute()` attaches the run
// via `detail::add_nested` (borrowed parent link for a caller-owned block), so the op settles
// only when the run does - grants held across, exactly the graph-node model. Covers the
// in-ctor inline arm returning with the op still in flight (`settled_sync_` bookkeeping) and
// the child-settle completion route (`release()` -> `settle_thunk`).
void test_op_nested_graph_run()
{
    ts::Guarded<int> x{ ts::Named{}, 0 };
    ts::Guarded<int> y{ ts::Named{}, 0 };
    std::atomic<bool> release_node{ false };
    std::atomic<bool> node_ran{ false };
    ts::Static_task_graph inner;
    inner.add_node(ts::Named{}, [&release_node, &node_ran](int& v)
    {
        while (!release_node.load())
            std::this_thread::yield();
        v = 5;
        node_ran.store(true);
    }, y);
    inner.compile();

    auto op = x.access([&inner](int& v)
    {
        v = 1;
        (void)inner.execute();   // gates the op's completion; the body returns with the run in flight
    });
    TS_CHECK(!op.is_done());   // inline arm ran the body, but the nested run still gates completion

    // The grant is held across the nested run: a concurrent write on `x` queues behind the op.
    std::atomic<bool> waiter_ran{ false };
    ts::Task<void> waiter = x.async([&waiter_ran](int&) { waiter_ran.store(true); });
    std::this_thread::sleep_for(5ms);
    TS_CHECK(!waiter_ran.load());

    release_node.store(true);
    op.sync();   // settles only after the inner run
    TS_CHECK(node_ran.load());
    waiter.sync();
    TS_CHECK(waiter_ran.load());
    TS_CHECK(read_value(x) == 1);
}

} // namespace

void run_guarded_tests()
{
    std::printf("\n[guarded] tests\n");
    run("type constraints", test_type_constraints);
    run("construct", test_construct);
    run("write then read", test_write_then_read);
    run("async returns value", test_async_returns_value);
    run("destructor waits", test_destructor_waits);
    run("sync then destroy", test_sync_then_destroy);
    run("multi-object sync then destroy", test_multi_sync_then_destroy);
    run("death: multi-object duplicate object", []{ TS_CHECK(ts::test::expect_death("async_duplicate_object")); });
    run("serial correctness", test_serial_correctness);
    run("concurrent readers", test_concurrent_readers);
    run("writer exclusion", test_writer_exclusion);
    run("reader after writer", test_reader_after_writer);
    run("R,W,R group separation", test_rwr_group_separation);
    run("writer FIFO", test_writer_fifo);
    run("independent objects parallel", test_independent_objects_parallel);
    run("generic readers overlap", test_generic_readers_overlap);
    run("generic writer serializes", test_generic_writer_serializes);
    run("reentrant same object", test_reentrant_same_object);
    run("reentrant other object", test_reentrant_other_object);
    run("pipe stress", test_pipe_stress);
    run("access runs synchronously", test_access_runs_synchronously);
    run("access read on caller", test_access_read_on_caller);
    run("access falls back when busy", test_access_falls_back_when_busy);
    run("async always schedules", test_async_always_schedules);
    run("async read schedules", test_async_read_schedules);
    run("op: void result", test_op_void_result);
    run("op: member storage", test_op_member_storage);
    run("op: try_take", test_op_try_take);
    run("op: as_optional", test_op_as_optional);
    run("op: cancellation", test_op_cancellation);
    run("op: awaited across suspension", test_op_await_suspending);
    run("op: named form awaited", test_op_await_named);
    run("op: dtor waits out unsettled", test_op_dtor_waits_unsettled);
    run("op: bind then start", test_op_lifecycle_bind_start);
    run("op: refire loop", test_op_refire);
    run("op: rebind settled to another object", test_op_rebind_settled);
    run("op: nested graph run gates completion", test_op_nested_graph_run);
    run("op: sync/take vocabulary", test_op_sync_take_vocabulary);
    run("multi-op: basic and refire", test_multi_op_basic);
    run("multi-op: declaration order", test_multi_op_declaration_order);
    run("multi-op: awaited across suspension", test_multi_op_await);
    run("multi-op: inline when all free", test_multi_op_inline_when_all_free);
    run("multi-op: defers when one busy", test_multi_op_defers_when_one_busy);
    run("multi-op: never jumps a queued entry", test_multi_op_never_jumps_queued_entry);
    run("multi-op: readers overlap", test_multi_op_readers_overlap);
    run("multi-op: opposite orders stress", test_multi_op_opposite_orders_stress);
    run("multi-op: lends the held object", test_multi_op_lend_partial);
    run("multi-op: all lent runs inline", test_multi_op_lend_all);
    run("death: multi-object access duplicate object",
        []{ TS_CHECK(ts::test::expect_death("access_duplicate_object")); });
    run_if(ts::test::with_harness, "TS_SAFETY_CHECKS=0", "death: access writes under a read grant",
        []{ TS_CHECK(ts::test::expect_death("access_write_under_read_grant")); });
    run("op: queued option", test_op_queued_option);
    run("async inherits caller priority", test_async_inherits_caller_priority);
    run_if(ts::test::with_harness, "TS_SAFETY_CHECKS=0", "death: op start unbound",
        []{ TS_CHECK(ts::test::expect_death("access_op_start_unbound")); });
    run_if(ts::test::with_harness, "TS_SAFETY_CHECKS=0", "death: op sync never started",
        []{ TS_CHECK(ts::test::expect_death("access_op_sync_never_started")); });
    run_if(ts::test::with_harness, "TS_SAFETY_CHECKS=0", "death: op double consume",
        []{ TS_CHECK(ts::test::expect_death("access_op_double_consume")); });
    run_if(ts::test::with_harness, "TS_SAFETY_CHECKS=0", "death: op try_take never started",
        []{ TS_CHECK(ts::test::expect_death("access_op_try_take_never_started")); });
    run_if(ts::test::with_rule_in_task_sync, "TS_RULE_IN_TASK_SYNC off", "death: op dtor in task",
        []{ TS_CHECK(ts::test::expect_death("access_op_dtor_in_task")); });
}
