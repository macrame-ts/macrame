#include "scheduler_scope.h"
#include "pipe_tests.h"
#include "ts/guarded.h"
#include "ts/scheduler.h"
#include "ts/static_task_graph.h"
#include "harness.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>
#include <vector>

using ts::test::run;
using tests::Rw_probe;
using tests::Parallel_gate;

namespace
{

// Read the probe's post-run oracle state through a read grant (the query methods do not
// assert access, but the pipe read is the sanctioned way to touch the instance).
bool probe_violated(ts::Guarded<Rw_probe>& p)
{
    return p.async([](const Rw_probe& r) { return r.violated(); }).sync();
}
int probe_writes(ts::Guarded<Rw_probe>& p)
{
    return p.async([](const Rw_probe& r) { return r.writes(); }).sync();
}

int read_int(ts::Guarded<int>& d)
{
    return d.async([](const int& v) { return v; }).sync();
}

// --- A: reader/writer invariant -------------------------------------------

// A1: a group of readers issued between two writers all run concurrently - exercises the
// reader-group formation. The gate is met only if all `k` were in flight at once (fails,
// does not hang, if the pipe serialized them).
void test_readers_overlap_in_group()
{
    constexpr int k = 4;
    Parallel_gate gate{ k };
    ts::Guarded<int> data{ ts::Named{}, 0 };

    data.async([](int& v) { v = 1; });   // a writer opens the sequence
    std::vector<ts::Task<int>> reads;
    for (int i = 0; i < k; ++i)
    {
        reads.push_back(data.async([&gate](const int& v)
        {
            gate.arrive();
            return v;
        }));
    }
    data.async([](int& v) { v = 2; });   // a writer closes it

    for (auto& t : reads)
        t.sync();
    TS_CHECK(gate.met());   // the reader group ran concurrently
}

// A2: writer exclusion via the dual oracle - the explicit invariant plus the
// unsynchronized payload (the latter is what TSan flags on a true overlap).
void test_writer_exclusion_probe()
{
    ts::Guarded<Rw_probe> probe{ ts::Named{} };
    std::vector<ts::Task<void>> tasks;

    int writes = 0;
    for (int round = 0; round < 30; ++round)
    {
        for (int r = 0; r < 4; ++r)
        {
            std::uint32_t seed = static_cast<std::uint32_t>(round * 8 + r);
            tasks.push_back(probe.async([seed](const Rw_probe& p) { p.observe_read(seed); }));
        }
        std::uint32_t wseed = static_cast<std::uint32_t>(round * 8 + 7);
        tasks.push_back(probe.async([wseed](Rw_probe& p) { p.observe_write(wseed); }));
        ++writes;
    }

    for (auto& t : tasks)
        t.sync();
    TS_CHECK(!probe_violated(probe));
    TS_CHECK(probe_writes(probe) == writes);
}

// A3: many threads issue a randomized read/write x async/access mix on one object. The
// oracle asserts the reader/writer invariant held; the write count is exact; TSan watches
// the payload. The cross-thread interleaving maximises overlap.
void test_mixed_interleave()
{
    ts::Guarded<Rw_probe> probe{ ts::Named{} };
    constexpr int producers = 6;
    constexpr int ops = 400;

    std::atomic<int> total_writes{ 0 };
    std::vector<std::thread> threads;
    for (int t = 0; t < producers; ++t)
    {
        threads.emplace_back([&probe, &total_writes, t]
        {
            std::vector<ts::Task<void>> local;
            int my_writes = 0;
            for (int i = 0; i < ops; ++i)
            {
                std::uint32_t seed = static_cast<std::uint32_t>(t * 100000 + i);
                bool write = (seed % 3) == 0;   // ~1/3 writes
                bool inl = (seed % 2) == 0;     // half via access (may run inline)
                if (write)
                {
                    ++my_writes;
                    if (inl)
                        probe.access([seed](Rw_probe& p) { p.observe_write(seed); }).sync();
                    else
                        local.push_back(probe.async([seed](Rw_probe& p) { p.observe_write(seed); }));
                }
                else
                {
                    if (inl)
                        probe.access([seed](const Rw_probe& p) { p.observe_read(seed); }).sync();
                    else
                        local.push_back(probe.async([seed](const Rw_probe& p) { p.observe_read(seed); }));
                }
            }
            for (auto& task : local)
                task.sync();
            total_writes.fetch_add(my_writes, std::memory_order_relaxed);
        });
    }
    for (auto& th : threads)
        th.join();

    TS_CHECK(!probe_violated(probe));
    TS_CHECK(probe_writes(probe) == total_writes.load());
}

// A4: reader concurrency at several degrees - the pipe admits k concurrent readers.
void test_reader_concurrency_degrees()
{
    for (int k : { 2, 4, 8 })
    {
        Parallel_gate gate{ k };
        ts::Guarded<int> data{ ts::Named{}, 0 };
        std::vector<ts::Task<int>> reads;
        for (int i = 0; i < k; ++i)
            reads.push_back(data.async([&gate](const int& v) { gate.arrive(); return v; }));
        for (auto& t : reads)
            t.sync();
        TS_CHECK(gate.met());
    }
}

// --- C: reader-group / ordering -------------------------------------------

// C1: readers issued after a writer form one group that overlaps; a writer issued after
// them waits for the whole group. Distinct from A1 by also asserting the trailing writer
// observed no concurrent reader.
void test_group_between_writers()
{
    constexpr int k = 4;
    Parallel_gate gate{ k };
    ts::Guarded<Rw_probe> probe{ ts::Named{} };

    probe.async([](Rw_probe& p) { p.observe_write(0); });
    std::vector<ts::Task<void>> reads;
    for (int i = 0; i < k; ++i)
    {
        std::uint32_t seed = static_cast<std::uint32_t>(i + 1);
        reads.push_back(probe.async([&gate, seed](const Rw_probe& p) { gate.arrive(); p.observe_read(seed); }));
    }
    ts::Task<void> closing = probe.async([](Rw_probe& p) { p.observe_write(99); });

    for (auto& t : reads)
        t.sync();
    closing.sync();
    TS_CHECK(gate.met());
    TS_CHECK(!probe_violated(probe));   // the closing writer waited for the group to drain
}

// C3: a reader issued after a writer runs strictly after it (the backout outcome - a
// reader never joins a group a writer has closed). The race-forcing variant is the C2
// stress; this asserts the ordering guarantee black-box.
void test_reader_after_writer_ordering()
{
    ts::Guarded<int> data{ ts::Named{}, 0 };
    data.async([](int& v) { v = 5; });
    ts::Task<int> after = data.async([](const int& v) { return v; });
    TS_CHECK(after.sync() == 5);
}

// --- D: lone-reader elision (behavioural half) ----------------------------

// D2: a lone reader in flight, then a second reader joins - both run concurrently
// (promotion to a group). A following writer waits for both. The white-box "no sentinel
// allocated for the lone reader" assert (D1) lands with the rewrite.
void test_promotion_two_readers()
{
    Parallel_gate gate{ 2 };
    ts::Guarded<Rw_probe> probe{ ts::Named{} };

    ts::Task<void> r1 = probe.async([&gate](const Rw_probe& p) { gate.arrive(); p.observe_read(1); });
    ts::Task<void> r2 = probe.async([&gate](const Rw_probe& p) { gate.arrive(); p.observe_read(2); });
    ts::Task<void> w = probe.async([](Rw_probe& p) { p.observe_write(3); });

    r1.sync(); r2.sync(); w.sync();
    TS_CHECK(gate.met());
    TS_CHECK(!probe_violated(probe));
}

// --- G: lifetime ----------------------------------------------------------

// G1: the destructor drains in-flight work. A `Guarded` is dropped immediately after
// launching writes; the loop widens the window under ASan/TSan.
void test_destructor_drains()
{
    for (int iter = 0; iter < 200; ++iter)
    {
        ts::Guarded<int> d{ ts::Named{}, 0 };
        for (int i = 0; i < 8; ++i)
            d.async([](int& v) { ++v; });
        // ~Guarded here must wait for the 8 writes.
    }
    TS_CHECK(true);   // reaching here without ASan/TSan report is the assertion
}

// G2: the last job completing races the destructor observing "idle" and freeing the pipe.
// The drain signal must outlive the pipe (UE's shared-ref-before-the-last-decrement).
void test_last_decrement_lifetime()
{
    for (int iter = 0; iter < 300; ++iter)
    {
        auto d = std::make_unique<ts::Guarded<int>>(ts::Named{}, 0);
        std::vector<ts::Task<void>> tasks;
        for (int i = 0; i < 16; ++i)
            tasks.push_back(d->async([](int& v) { ++v; }));
        // Drop the handles and the object while the last job may still be settling.
        tasks.clear();
        d.reset();
    }
    TS_CHECK(true);
}

// G3: a predecessor can complete and free the just-enqueued block before the enqueue
// returns - rapid enqueue+immediate-complete churn exercises the push-UAF bracket.
void test_push_uaf_churn()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };
    constexpr int n = 20000;
    for (int i = 0; i < n; ++i)
        d.async([](int& v) { ++v; });   // handle dropped immediately; each may free mid-push
    TS_CHECK(read_int(d) == n);
}

// G4: a cancelled writer still advances the pipe - its successor runs and sees the prior
// (unmodified) value; no hang.
void test_cancelled_writer_advances()
{
    ts::Guarded<int> d{ ts::Named{}, 7 };
    ts::Cancellation_source src;
    src.request_cancel();
    ts::Task<void> cancelled = d.async([](int& v) { v = 999; }, { .token = src.token() });
    ts::Task<int> after = d.async([](const int& v) { return v; });
    TS_CHECK(after.sync() == 7);   // the cancelled write was skipped; the chain advanced
}

// --- H: worker-less mode --------------------------------------------------

// H1: a deterministic read/write sequence on a worker-less scheduler yields identical
// results (bodies run inline at submit; the pipe still serializes).
void test_worker_less_deterministic()
{
    ts::Scheduler_scope scope{ ts::Scheduler_config{ .single_threaded = true } };
    ts::Guarded<int> d{ ts::Named{}, 0 };
    for (int i = 0; i < 100; ++i)
        d.async([](int& v) { ++v; });
    for (int i = 0; i < 50; ++i)
        d.async([](int& v) { v += 2; });
    TS_CHECK(read_int(d) == 200);
}

// H2: a deep piped write chain worker-less must not overflow the stack - inline-at-submit
// chains ride the bounded trampoline rather than recursing.
void test_worker_less_deep_chain()
{
    ts::Scheduler_scope scope{ ts::Scheduler_config{ .single_threaded = true } };
    ts::Guarded<int> d{ ts::Named{}, 0 };
    constexpr int n = 200000;
    for (int i = 0; i < n; ++i)
        d.async([](int& v) { ++v; });
    TS_CHECK(read_int(d) == n);
}

// Cross-arity: multi-object `ts::access` ops against graph nodes over the SAME pair of
// objects. Both ride the one canonical cascade, and the op's inline arm admits its whole set
// under the pipes' own mutexes, so the two must still serialize - the `Rw_probe` oracle fails
// on any overlap of a writer with a reader or another writer.
void test_graph_multi_access_hammer()
{
    ts::Guarded<Rw_probe> a{ ts::Named{ "a" } }, b{ ts::Named{ "b" } };
    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](Rw_probe& p, const Rw_probe& q) { p.observe_write(1); q.observe_read(1); }, a, b);
    g.add_node(ts::Named{}, [](const Rw_probe& q) { q.observe_read(2); }, b);
    g.compile();

    std::atomic<bool> stop{ false };
    std::vector<std::thread> hammers;
    for (int t = 0; t < 3; ++t)
    {
        hammers.emplace_back([&a, &b, &stop, t]
        {
            std::uint32_t s = static_cast<std::uint32_t>(t) * 7919u;
            while (!stop.load(std::memory_order_relaxed))
            {
                // Attended, so each op is consumed before the next: no in-flight window to
                // bound, and a wedge shows up as the main thread's deadline below.
                (void)ts::access([s](Rw_probe& p, const Rw_probe& q)
                {
                    p.observe_write(s);
                    q.observe_read(s);
                }, a, b).sync();
                (void)ts::access([s](const Rw_probe& p, const Rw_probe& q)
                {
                    return p.observe_read(s) + q.observe_read(s);
                }, a, b).sync();
                ++s;
            }
        });
    }
    for (int r = 0; r < 60; ++r)
    {
        ts::Task<void> run = g.execute();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!run.is_done() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        if (!run.is_done())
        {
            std::printf("MULTI-ACCESS HAMMER HANG at run %d\n", r);
            std::fflush(stdout);
            std::_Exit(3);
        }
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& h : hammers)
        h.join();

    TS_CHECK(!probe_violated(a));
    TS_CHECK(!probe_violated(b));
}

// E5-shape hammer (the tsan `stress_pipe_reservation` shape, natively): repeated graph
// runs over shared objects while spinning threads fire async reads/writes at the same
// objects. Guards the graph-links + reader-run + join interplay under real contention;
// completion of every run and async (no hang, no invariant break) is the assertion.
void test_graph_async_hammer()
{
    ts::Guarded<Rw_probe> a{ ts::Named{ "a" } }, b{ ts::Named{ "b" } };
    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [](Rw_probe& p) { p.observe_write(1); }, a);
    g.add_node(ts::Named{}, [](const Rw_probe& p) { p.observe_read(2); }, a);
    g.add_node(ts::Named{}, [](Rw_probe& p) { p.observe_write(3); }, b);
    g.compile();

    std::atomic<bool> stop{ false };
    std::vector<std::thread> hammers;
    for (int t = 0; t < 3; ++t)
    {
        hammers.emplace_back([&a, &b, &stop, t]
        {
            std::uint32_t s = static_cast<std::uint32_t>(t) * 7919u;
            // Bounded in-flight window: a wedged pipe once piled ~30M fire-and-forget
            // entries (multi-GB; the OOM took the host down). Retain handles and wait for
            // the oldest before pushing more, so a wedge saturates at `window` entries and
            // the main thread's deadline handles the diagnosis.
            constexpr std::size_t window = 64;
            std::deque<ts::Task<void>> inflight;
            while (!stop.load(std::memory_order_relaxed))
            {
                while (inflight.size() >= window && !stop.load(std::memory_order_relaxed))
                {
                    if (inflight.front().is_done())
                        inflight.pop_front();
                    else
                        std::this_thread::yield();
                }
                if (inflight.size() >= window)
                    break;   // stopped while saturated (wedge): drop the handles and exit
                inflight.push_back(a.async([s](const Rw_probe& p) { p.observe_read(s); }));
                inflight.push_back(b.async([s](Rw_probe& p) { p.observe_write(s); }));
                ++s;
            }
        });
    }
    // Bounded sync: a wedged run must fail the suite (exit 3), not hang it - returning
    // with the hammers live would crash, and a silent hang blocks the whole run.
    for (int r = 0; r < 60; ++r)
    {
        ts::Task<void> run = g.execute();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!run.is_done() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        if (!run.is_done())
        {
            std::printf("HAMMER HANG at run %d\n", r);
            std::fflush(stdout);
            std::_Exit(3);
        }
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& h : hammers)
        h.join();

    TS_CHECK(!probe_violated(a));
    TS_CHECK(!probe_violated(b));
}

// --- J: priority ----------------------------------------------------------

// J1: pipe order is not reordered by priority - a low-priority write followed by a
// high-priority read still sees the write (the pipe edge dominates the queue priority).
void test_priority_does_not_reorder()
{
    ts::Guarded<int> d{ ts::Named{}, 0 };
    d.async([](int& v) { v = 3; }, { .priority = ts::Priority::low });
    ts::Task<int> hi = d.async([](const int& v) { return v; }, { .priority = ts::Priority::high });
    TS_CHECK(hi.sync() == 3);
}

// --- F: grant ownership (`Pipe::writer_owner`) ----------------------------
//
// White-box, because `writer_owner` is the one always-on piece of grant state and behavior
// keys off it: `Deferred::commit()` applies inline exactly when the caller is the holder,
// and `Guarded::access` publishes it for an inline body. Those two verbs are
// covered end-to-end elsewhere; these pin the invariant itself, so a regression is reported
// here rather than as a mysterious extra write job or a deadlock.

// The block currently holding this object's write grant, or null.
template<typename T>
ts::detail::Task_control_block* owner_of(ts::Guarded<T>& obj)
{
    return ts::detail::Guarded_access::pipe(obj).writer_owner.load(std::memory_order_acquire);
}

// Ownership is cleared asynchronously with respect to a waiter: `sync()` returns once the
// block settles, but the pipe release runs in the block's `on_complete`, at the very end of
// settle - after `done_cv.notify_all()` has already woken the waiter. So a `sync()`ing
// thread can legitimately observe the still-set owner, and "the write grant was released"
// has to be polled rather than read once. (That ordering is itself worth pinning: it is why
// a caller must not infer pipe state from a task's completion.)
template<typename T>
bool owner_cleared(ts::Guarded<T>& obj)
{
    for (int spins = 0; spins < 1'000'000 && owner_of(obj) != nullptr; ++spins)
        std::this_thread::yield();
    return owner_of(obj) == nullptr;
}

// F1: a write body sees itself as the owner; outside any write window the owner is null.
// The read case is the other half of the invariant - a reader hold must not publish an
// owner, or `commit()` would take its inline arm under a read grant.
void test_writer_owner_set_and_cleared()
{
    ts::Guarded<int> x{ ts::Named{}, 0 };
    TS_CHECK(owner_of(x) == nullptr);

    std::atomic<bool> matched{ false };
    x.async([&x, &matched](int& v)
    {
        v = 1;
        matched.store(owner_of(x) == ts::detail::Current_task::get());
    }).sync();

    TS_CHECK(matched.load());
    TS_CHECK(owner_cleared(x));   // released with the write

    std::atomic<void*> during_read{ reinterpret_cast<void*>(1) };
    x.async([&x, &during_read](const int&) { during_read.store(owner_of(x)); }).sync();
    TS_CHECK(during_read.load() == nullptr);
}

// F2 (re-scoped for the evolved pipe - the explicit graph write handoff this originally
// tested is deleted): ownership transfers by release-then-admit. Two chained writes must
// each name their own block, never the predecessor's and never null.
void test_writer_owner_transfers_between_writes()
{
    ts::Guarded<int> x{ ts::Named{}, 0 };
    std::atomic<void*> first{ nullptr }, second{ nullptr };
    std::atomic<bool> self1{ false }, self2{ false };

    ts::Task<void> a = x.async([&](int& v)
    {
        v += 1;
        first.store(owner_of(x));
        self1.store(owner_of(x) == ts::detail::Current_task::get());
    });
    ts::Task<void> b = x.async([&](int& v)
    {
        v += 1;
        second.store(owner_of(x));
        self2.store(owner_of(x) == ts::detail::Current_task::get());
    });
    a.sync();
    b.sync();

    TS_CHECK(self1.load() && self2.load());
    TS_CHECK(first.load() != nullptr && second.load() != nullptr);
    TS_CHECK(first.load() != second.load());   // the release cleared it; the next admission set it
    TS_CHECK(owner_cleared(x));
}

// F3: the inline arms. An `access` on a free pipe runs on the caller's thread but is still a
// real admission, so it publishes its own block as the owner for the body's duration. The
// lent half is the interesting one: an `access` from a task that already holds the
// write grant runs under that grant and touches the pipe not at all, so the owner must stay
// the outer block - if it were republished (or cleared on the inner settle) `commit()`
// would mis-dispatch for the rest of the outer body.
void test_writer_owner_inline_and_reentrant()
{
    ts::Guarded<int> x{ ts::Named{}, 0 };

    std::atomic<void*> inline_owner{ nullptr };
    x.access([&](int& v) { v = 1; inline_owner.store(owner_of(x)); }).sync();
    TS_CHECK(inline_owner.load() != nullptr);
    TS_CHECK(owner_cleared(x));

    std::atomic<void*> outer{ nullptr }, inner{ nullptr }, after{ nullptr };
    std::atomic<bool> inner_ran{ false };
    x.async([&](int& v)
    {
        outer.store(owner_of(x));
        auto nested = x.access([&](int& w) { w += 1; inner_ran.store(true); inner.store(owner_of(x)); });
        TS_CHECK(nested.is_done());   // reentrant: ran inline, in-call
        after.store(owner_of(x));
        v += 1;
    }).sync();

    TS_CHECK(inner_ran.load());
    TS_CHECK(outer.load() != nullptr);
    TS_CHECK(inner.load() == outer.load());   // ran under the outer grant, owner unchanged
    TS_CHECK(after.load() == outer.load());   // and the inner settle did not clear it
    TS_CHECK(owner_cleared(x));
}

// F4: a multi-object write holds several pipes at once; each names the same block, and each
// is independent (a third object the task never touched stays unowned).
void test_writer_owner_multi_object()
{
    ts::Guarded<int> a{ ts::Named{}, 0 }, b{ ts::Named{}, 0 }, c{ ts::Named{}, 0 };
    std::atomic<bool> both_self{ false };
    std::atomic<void*> untouched{ reinterpret_cast<void*>(1) };

    ts::async([&](int& x, int& y)
    {
        x = 1;
        y = 2;
        ts::detail::Task_control_block* self = ts::detail::Current_task::get();
        both_self.store(owner_of(a) == self && owner_of(b) == self);
        untouched.store(owner_of(c));
    }, a, b).sync();

    TS_CHECK(both_self.load());
    TS_CHECK(untouched.load() == nullptr);
    TS_CHECK(owner_cleared(a) && owner_cleared(b));
}

} // namespace

void run_pipe_tests()
{
    run("readers overlap in group", test_readers_overlap_in_group);
    run("writer exclusion probe", test_writer_exclusion_probe);
    run("mixed interleave", test_mixed_interleave);
    run("reader concurrency degrees", test_reader_concurrency_degrees);
    run("group between writers", test_group_between_writers);
    run("reader after writer ordering", test_reader_after_writer_ordering);
    run("promotion two readers", test_promotion_two_readers);
    run("destructor drains", test_destructor_drains);
    run("last decrement lifetime", test_last_decrement_lifetime);
    run("push uaf churn", test_push_uaf_churn);
    run("cancelled writer advances", test_cancelled_writer_advances);
    run("graph async hammer", test_graph_async_hammer);
    run("graph multi-access hammer", test_graph_multi_access_hammer);
    run("worker-less deterministic", test_worker_less_deterministic);
    run("worker-less deep chain", test_worker_less_deep_chain);
    run("priority does not reorder", test_priority_does_not_reorder);
    run("writer_owner set and cleared", test_writer_owner_set_and_cleared);
    run("writer_owner transfers between writes", test_writer_owner_transfers_between_writes);
    run("writer_owner inline + reentrant", test_writer_owner_inline_and_reentrant);
    run("writer_owner multi-object", test_writer_owner_multi_object);
}
