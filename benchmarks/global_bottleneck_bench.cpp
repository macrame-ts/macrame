#include "benchmarks.h"
#include "bench_harness.h"

#include "ts/guarded.h"
#include "ts/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace
{

// --- global bottleneck flood: short-scope std::mutex vs Guarded, under task flood -----
//
// The wild pattern this models: task-parallel code where every task briefly touches one
// global shared store under a mutex held for the smallest possible scope (a registry, a
// stats block, a shared list). The flood keeps the scheduler's queue full, so any worker
// parked on the mutex idles a core while runnable tasks wait - the classic stall. The
// industry mitigation is an oversubscribed pool (more workers than cores): parked
// workers are backfilled at context-switch cost. The `Guarded` version of the same task
// leaves a contended touch queued on the store's pipe and moves to the next task, so the
// worker never parks and oversubscription has nothing to fix - the `grd 2x` row exists
// as the null check of exactly that claim.
//
// Row structure: worker count set per row via `Scheduler_scope{.num_threads}` - the
// scheduler is the pool, so `2x` is genuine oversubscription of the same machinery.
// Pools past 2x are rare in the wild and not benched. Per task: `W_out` of independent
// spin, then the touch (`W_crit` under the mutex / inside the `access` body). Two
// `W_crit` columns: 200 ns puts the global near saturation at hw workers (offered load
// ~ hw * W_crit/(W_out+W_crit)), 1 us puts it far past it (the serial chain is the
// ceiling). Completion is counted in the touch; both worlds pay one shared-line RMW per
// task and the same raw-submit flood (no task-block allocs on either side). The guarded
// row additionally pays one command block per contended touch - today's `access` cost,
// the `Access_op` before-story - and its pipe backlog peaks at tens of thousands of
// queued blocks in the 1 us column (~10 MB, transient).
//
// Honesty notes: a raw `std::mutex::lock` inside a task body is the documented
// *uncovered* blocking case (opaque to the library, no fatal) - which is precisely the
// pattern being modeled. Ceilings printed per column: serial bound 1/W_crit and core
// bound hw_cores/(W_out+W_crit); the binding one is the smaller. No row may exceed its
// ceiling.

constexpr int flood_batch = 50000;
constexpr int w_out_ns = 5000;

void spin_ns(int ns)
{
    if (ns <= 0)
        return;
    auto end = Clock::now() + std::chrono::nanoseconds(ns);
    while (Clock::now() < end)
    {
    }
}

struct alignas(64) Global_mutex_store
{
    std::mutex m;
    std::uint64_t v = 0;
};

struct Global_payload
{
    std::uint64_t v = 0;
};

struct Mtx_ctx
{
    Global_mutex_store* store;
    std::atomic<int>* done;
    int crit_ns;
};

void mtx_task(void* p)
{
    auto* c = static_cast<Mtx_ctx*>(p);
    spin_ns(w_out_ns);
    {
        std::scoped_lock lock(c->store->m);
        spin_ns(c->crit_ns);
        ++c->store->v;
    }
    c->done->fetch_add(1, std::memory_order_release);
}

std::vector<double> bench_mtx_flood(unsigned workers, int crit_ns)
{
    ts::Scheduler_scope scope{ { .num_threads = workers } };
    ts::Scheduler& sched = ts::global_scheduler();
    Global_mutex_store store;
    std::atomic<int> done{ 0 };
    Mtx_ctx ctx{ &store, &done, crit_ns };

    return measure([&]() -> uint64_t
    {
        done.store(0, std::memory_order_relaxed);
        for (int i = 0; i < flood_batch; ++i)
            sched.submit(&mtx_task, &ctx);
        while (done.load(std::memory_order_acquire) != flood_batch)
            std::this_thread::yield();
        return flood_batch;
    });
}

struct Grd_ctx
{
    ts::Guarded<Global_payload>* store;
    std::atomic<int>* done;
    int crit_ns;
};

void grd_task(void* p)
{
    auto* c = static_cast<Grd_ctx*>(p);
    spin_ns(w_out_ns);
    auto* done = c->done;
    int crit = c->crit_ns;
    (void)c->store->access([done, crit](Global_payload& g)
    {
        spin_ns(crit);
        ++g.v;
        done->fetch_add(1, std::memory_order_release);
    });
}

std::vector<double> bench_grd_flood(unsigned workers, int crit_ns)
{
    ts::Scheduler_scope scope{ { .num_threads = workers } };
    ts::Scheduler& sched = ts::global_scheduler();
    ts::Guarded<Global_payload> store{ ts::Named{} };
    std::atomic<int> done{ 0 };
    Grd_ctx ctx{ &store, &done, crit_ns };

    return measure([&]() -> uint64_t
    {
        done.store(0, std::memory_order_relaxed);
        for (int i = 0; i < flood_batch; ++i)
            sched.submit(&grd_task, &ctx);
        while (done.load(std::memory_order_acquire) != flood_batch)
            std::this_thread::yield();
        return flood_batch;
    });
}

void print_ceilings(int crit_ns, unsigned hw)
{
    double serial = 1e3 / crit_ns;                                       // M/s
    double core = hw * 1e3 / static_cast<double>(w_out_ns + crit_ns);    // M/s
    std::printf("  crit=%dns (serial bound %.2f M/s, core bound %.2f M/s):\n",
        crit_ns, serial, core);
}

} // namespace

void run_global_bottleneck_bench()
{
    unsigned hw = std::thread::hardware_concurrency();
    std::printf("\nglobal bottleneck flood (one shared store, smallest-scope touches; W_out=%dns,"
                " %d tasks/batch):\n", w_out_ns, flood_batch);

    for (int crit : { 200, 1000 })
    {
        print_ceilings(crit, hw);
        report("mtx hw", bench_mtx_flood(hw, crit));
        report("mtx 2x", bench_mtx_flood(2 * hw, crit));
        report("grd hw", bench_grd_flood(hw, crit));
        report("grd 2x", bench_grd_flood(2 * hw, crit));   // the null check: expect flat-to-worse
    }
}
