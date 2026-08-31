// Lazily-built shared state - the "build it on first use, from several tasks at once" case
// (TBB reaches for `collaborative_call_once` here). Ray casts run concurrently and all need a
// BVH that has not been built yet.
//
// The library needs no call-once primitive: the BVH is guarded, so one cast builds it under a
// write grant while the others wait. Waiting is a suspension, not a blocked thread - the
// waiting casts hand their workers back, and the build then goes wide through `parallel_for` on
// precisely those workers. The waiters do not join the build the way `collaborative_call_once`
// makes them; the builder recruits the pool from the other side, which keeps the cores busy
// just the same.
//
// Three things make the pattern work, and each is a library rule rather than something the
// sample arranges:
//
//  - The built flag lives INSIDE the guarded type, so `is_built()` is harness-checked like
//    every other entry point. A `Guarded<std::optional<BVH>>` would put that check outside any
//    object the harness knows about.
//  - A read grant is never upgraded in place - two readers upgrading at once deadlock - so the
//    check hands its grant back before the build is requested. Holding a guard across a
//    `co_await` is fatal anyway, which makes separate accesses the only legal shape as well as
//    the simplest.
//  - No graph and no fan-out primitive is needed. The casts start eagerly on the creating
//    thread, but the first suspends at the build it requests (`async` always schedules), so the
//    loop keeps going; the rest then queue behind that build rather than slipping in as
//    readers, because queued work is never jumped.
//
// If the need for the BVH were known up front, none of this belongs here: a build node that the
// casting nodes declare a read against gets a derived edge instead of a race. This sample is
// the residual case, where the need is discovered at run time.

#include "ts/ts.h"

#include <cstdio>
#include <vector>

namespace sample
{
namespace
{

// The build has to be two things for the pattern to be visible: expensive, so the other casts
// reach the object while it is still running, and split into enough pieces for `parallel_for`
// to spread across the pool. `build_pieces` is that split - `parallel_for` needs an item count,
// and any number comfortably above the worker count will do. A query is cheap by comparison,
// which is why they can all run concurrently once the build is done.
constexpr int ray_count = 16;
constexpr int build_pieces = 512;
constexpr int build_work = 2000;
constexpr int query_work = 50;

// `volatile` so the loop survives optimization; without it there is no build to wait for.
void spin(int iterations)
{
    volatile int sink = 0;
    for (int i = 0; i < iterations; ++i)
        sink = sink + i;
}

// A stand-in: the methods are a harness check plus busy work, since the pattern is about when
// they run, not what they compute. The built flag is the one piece of real state - it is what
// every caller has to check, and having it here is what puts that check behind the harness.
class BVH
{
public:
    bool is_built() const
    {
        TS_CHECK_ACCESS();
        return built_;
    }

    // Called under a write grant, so this task is alone in the object. The build fans out with
    // `parallel_for`, whose helpers inherit this task's grant and run on the workers the
    // waiting casts freed by suspending.
    void build()
    {
        TS_CHECK_ACCESS();
        ts::parallel_for(build_pieces, [](int) { spin(build_work); });
        built_ = true;
    }

    void closest_hit() const
    {
        TS_CHECK_ACCESS();
        spin(query_work);
    }

private:
    bool built_ = false;
};

ts::Task<void> cast_ray(ts::Guarded<BVH>& bvh)
{
    if (!co_await bvh.access([](const BVH& b) { return b.is_built(); }))
        co_await bvh.async([](BVH& b) { if (!b.is_built()) b.build(); });   // double-check under the write

    auto view = co_await ts::read_only(bvh);   // concurrent with every other cast
    view->closest_hit();
}

bool cast_all()
{
    ts::Guarded<BVH> bvh{ ts::Named{ "bvh" } };

    std::vector<ts::Task<void>> casts;
    for (int ray = 0; ray < ray_count; ++ray)
        casts.push_back(cast_ray(bvh));
    for (ts::Task<void>& cast : casts)
        cast.sync();   // outside any task, so blocking here is the sanctioned boundary verb

    // Every cast completed, so the object must have ended up built. That it was built exactly
    // once is structural rather than checked here: `build()` runs only under the exclusive
    // write grant, behind the double-check.
    return bvh.access([](const BVH& b) { return b.is_built(); }).sync();
}

} // namespace

// Headless entry for the sanitizer stress driver: concurrent guard acquisition plus a
// `parallel_for` under a write grant is worth running under ThreadSanitizer.
void stress_lazy_BVH(int frames)
{
    for (int i = 0; i < frames; ++i)
        (void)cast_all();
}

void run_lazy_BVH_sample()
{
    std::printf("lazy BVH sample: %d concurrent casts against a BVH built on first use, %s\n",
                ray_count, cast_all() ? "all completed with the BVH built" : "FAILED (bug)");
}

} // namespace sample
