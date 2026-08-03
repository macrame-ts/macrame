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

// A1: a group of readers issued between two writers all run concurrently -- exercises the
// reader-group formation. The gate is met only if all `k` were in flight at once (fails,
// does not hang, if the pipe serialized them).
void test_readers_overlap_in_group()
{
    constexpr int k = 4;
    Parallel_gate gate{ k };
    ts::Guarded<int> data{ 0 };

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

// A2: writer exclusion via the dual oracle -- the explicit invariant plus the
// unsynchronized payload (the latter is what TSan flags on a true overlap).
void test_writer_exclusion_probe()
{
    ts::Guarded<Rw_probe> probe;
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
// the payload. The cross-thread interleaving is the point -- maximises overlap.
void test_mixed_interleave()
{
    ts::Guarded<Rw_probe> probe;
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
                        probe.access([seed](Rw_probe& p) { p.observe_write(seed); });
                    else
                        local.push_back(probe.async([seed](Rw_probe& p) { p.observe_write(seed); }));
                }
                else
                {
                    if (inl)
                        probe.access([seed](const Rw_probe& p) { p.observe_read(seed); });
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

// A4: reader concurrency at several degrees -- the pipe admits k concurrent readers.
void test_reader_concurrency_degrees()
{
    for (int k : { 2, 4, 8 })
    {
        Parallel_gate gate{ k };
        ts::Guarded<int> data{ 0 };
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
    ts::Guarded<Rw_probe> probe;

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

// C3: a reader issued after a writer runs strictly after it (the backout outcome -- a
// reader never joins a group a writer has closed). The race-forcing variant is the C2
// stress; this asserts the ordering guarantee black-box.
void test_reader_after_writer_ordering()
{
    ts::Guarded<int> data{ 0 };
    data.async([](int& v) { v = 5; });
    ts::Task<int> after = data.async([](const int& v) { return v; });
    TS_CHECK(after.sync() == 5);
}

// --- D: lone-reader elision (behavioural half) ----------------------------

// D2: a lone reader in flight, then a second reader joins -- both run concurrently
// (promotion to a group). A following writer waits for both. The white-box "no sentinel
// allocated for the lone reader" assert (D1) lands with the rewrite.
void test_promotion_two_readers()
{
    Parallel_gate gate{ 2 };
    ts::Guarded<Rw_probe> probe;

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
        ts::Guarded<int> d{ 0 };
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
        auto d = std::make_unique<ts::Guarded<int>>(0);
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
// returns -- rapid enqueue+immediate-complete churn exercises the push-UAF bracket.
void test_push_uaf_churn()
{
    ts::Guarded<int> d{ 0 };
    constexpr int n = 20000;
    for (int i = 0; i < n; ++i)
        d.async([](int& v) { ++v; });   // handle dropped immediately; each may free mid-push
    TS_CHECK(read_int(d) == n);
}

// G4: a cancelled writer still advances the pipe -- its successor runs and sees the prior
// (unmodified) value; no hang.
void test_cancelled_writer_advances()
{
    ts::Guarded<int> d{ 7 };
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
    ts::Guarded<int> d{ 0 };
    for (int i = 0; i < 100; ++i)
        d.async([](int& v) { ++v; });
    for (int i = 0; i < 50; ++i)
        d.async([](int& v) { v += 2; });
    TS_CHECK(read_int(d) == 200);
}

// H2: a deep piped write chain worker-less must not overflow the stack -- inline-at-submit
// chains ride the bounded trampoline rather than recursing.
void test_worker_less_deep_chain()
{
    ts::Scheduler_scope scope{ ts::Scheduler_config{ .single_threaded = true } };
    ts::Guarded<int> d{ 0 };
    constexpr int n = 200000;
    for (int i = 0; i < n; ++i)
        d.async([](int& v) { ++v; });
    TS_CHECK(read_int(d) == n);
}

// E5-shape hammer (the tsan `stress_pipe_reservation` shape, natively): repeated graph
// runs over shared objects while spinning threads fire async reads/writes at the same
// objects. Guards the graph-links + reader-run + join interplay under real contention;
// completion of every run and async (no hang, no invariant break) is the assertion.
void test_graph_async_hammer()
{
    ts::Guarded<Rw_probe> a, b;
    ts::Static_task_graph g;
    g.add_node([](Rw_probe& p) { p.observe_write(1); }, a);
    g.add_node([](const Rw_probe& p) { p.observe_read(2); }, a);
    g.add_node([](Rw_probe& p) { p.observe_write(3); }, b);
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
                a.async([s](const Rw_probe& p) { p.observe_read(s); });
                b.async([s](Rw_probe& p) { p.observe_write(s); });
                ++s;
            }
        });
    }
#if TS_PIPE_TAIL
    // TEMP DIAGNOSTIC: bounded sync + state dump on hang (revert after the hang is fixed).
    // Tail-only fields, so the whole diagnostic (and the bounded-sync loop using it) is
    // gated; the flag-off build runs the plain loop below.
    auto dump_link = [](const char* tag, ts::detail::Pipe_link* l)
    {
        std::printf("    %s link=%p: mode=%d role=%d gate=%u next=%p tenure=%u claim=%d pin=%d idx=%d",
            tag, static_cast<void*>(l), static_cast<int>(l->mode), static_cast<int>(l->role.load()),
            l->gate.load(), reinterpret_cast<void*>(l->next.load()), l->tenure.load(),
            static_cast<int>(l->turn_claim.load()), static_cast<int>(l->join_pin.load()),
            static_cast<int>(l->index));
        if (l->owner)
            std::printf(" | owner=%p locks=%08x entered=%d pcount=%d ready=%d",
                static_cast<void*>(l->owner), l->owner->num_locks.load(),
                static_cast<int>(l->owner->pipes_entered), static_cast<int>(l->owner->pipe_count),
                static_cast<int>(l->owner->ready.load()));
        std::printf("\n");
    };
    auto dump_pipe = [&dump_link](const char* name, ts::detail::Pipe& p)
    {
        std::uintptr_t tw = p.tail.load();
        std::printf("  pipe %s: tail=%p count=%u\n", name, reinterpret_cast<void*>(tw), p.task_count.load());
        if (tw == 0)
            return;
        // Walk the custody chain backward to the FRONT of the line (each link's
        // `prev_owner` names its predecessor's block; find that block's link on this
        // pipe). The front region of a stuck line is quiescent, so this is safe enough
        // for a post-mortem dump.
        auto* l = reinterpret_cast<ts::detail::Pipe_link*>(tw & ~static_cast<std::uintptr_t>(63));
        long depth = 0;
        while (depth < 50000000)
        {
            ts::detail::Task_control_block* prev = l->prev_owner;
            if (prev == nullptr)
                break;
            ts::detail::Pipe_link* pl = nullptr;
            for (std::uint8_t k = 0; k < prev->pipe_count; ++k)
            {
                if (prev->pipe_links[k].pipe == &p)
                {
                    pl = &prev->pipe_links[k];
                    break;
                }
            }
            if (pl == nullptr)
                break;
            l = pl;
            ++depth;
        }
        std::printf("    depth-from-tail=%ld\n", depth);
        dump_link("front", l);
        // And the front's forward chain, a few hops.
        for (int hop = 0; hop < 3; ++hop)
        {
            std::uintptr_t nx = l->next.load();
            if (nx == 0 || (nx & 3) != 0)
            {
                std::printf("    next-word=%p\n", reinterpret_cast<void*>(nx));
                break;
            }
            l = reinterpret_cast<ts::detail::Pipe_link*>(nx);
            dump_link("fwd", l);
        }
    };
    for (int r = 0; r < 60; ++r)
    {
        ts::Task<void> run = g.execute();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!run.is_done() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        if (!run.is_done())
        {
            std::printf("HAMMER HANG at run %d\n", r);
            dump_pipe("a", ts::detail::Guarded_access::pipe(a));
            dump_pipe("b", ts::detail::Guarded_access::pipe(b));
            std::fflush(stdout);
            std::_Exit(3);
        }
    }
#else
    for (int r = 0; r < 60; ++r)
        g.execute().sync();
#endif
    stop.store(true, std::memory_order_relaxed);
    for (auto& h : hammers)
        h.join();

    TS_CHECK(!probe_violated(a));
    TS_CHECK(!probe_violated(b));
}

// --- J: priority ----------------------------------------------------------

// J1: pipe order is not reordered by priority -- a low-priority write followed by a
// high-priority read still sees the write (the pipe edge dominates the queue priority).
void test_priority_does_not_reorder()
{
    ts::Guarded<int> d{ 0 };
    d.async([](int& v) { v = 3; }, { .priority = ts::Priority::low });
    ts::Task<int> hi = d.async([](const int& v) { return v; }, { .priority = ts::Priority::high });
    TS_CHECK(hi.sync() == 3);
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
    run("worker-less deterministic", test_worker_less_deterministic);
    run("worker-less deep chain", test_worker_less_deep_chain);
    run("priority does not reorder", test_priority_does_not_reorder);
}
