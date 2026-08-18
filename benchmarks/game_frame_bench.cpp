#include "benchmarks.h"
#include "bench_harness.h"

#include <cstdio>
#include <thread>
#include <vector>

// The game-frame sample is a single self-contained .cpp (no header); the two compositions
// this benchmark compares are forward-declared. Same World, same system bodies: one
// scheduled by a compiled `Static_task_graph`, one hand-composed with coroutines.
namespace sample
{
void game_frame_stats(int frames, float time_scale, double& avg_ms, double& serial_ms, float& transform0);
void game_frame_free_stats(int frames, float time_scale, double& avg_ms, double& serial_ms, float& transform0);
}

namespace
{

// --- graph vs graph-free frame composition ---------------------------------
// Both entries run the same frame - same `World`, same tick_* bodies, same worker pool
// (the global scheduler) - differing only in how the schedule is produced: a compiled
// `Static_task_graph` (edges derived from the access declarations, re-runs allocate only
// the `done` handle) vs `run_frame_graph_free` (a coroutine per chain, one task block +
// pipe-link set per system per frame, every edge an explicit `co_await`). The graph's
// structural advantage is allocation amortization, so the interesting question is at what
// system granularity that becomes visible: `scale` shrinks every system's mock cost, and
// the fixed per-frame composition cost stays put.

using Frame_run = void (*)(int, float, double&, double&, float&);

double frame_us(Frame_run run, int frames, float scale)
{
    double avg_ms = 0.0, serial_ms = 0.0;
    float transform0 = 0.0f;
    run(frames, scale, avg_ms, serial_ms, transform0);   // warmup (also pages in the World)
    std::vector<double> us;
    for (int r = 0; r < reps; ++r)
    {
        run(frames, scale, avg_ms, serial_ms, transform0);
        us.push_back(avg_ms * 1000.0);
    }
    return median(std::move(us));
}

void report_frame(const char* name, double us_per_frame, double reference)
{
    if (reference > 0.0)
    {
        std::printf("  %-10s %9.1f us/frame   [%+6.1f%% vs graph]\n", name, us_per_frame,
            100.0 * (us_per_frame - reference) / reference);
    }
    else
    {
        std::printf("  %-10s %9.1f us/frame\n", name, us_per_frame);
    }
}

} // namespace

void run_game_frame_bench()
{
    unsigned hw = std::thread::hardware_concurrency();
    std::printf("\ngame frame, graph vs graph-free (same World, same system bodies, %u hw threads):\n", hw);
    double heavy_graph = frame_us(&sample::game_frame_stats, 20, 1.0f);
    double heavy_free = frame_us(&sample::game_frame_free_stats, 20, 1.0f);
    double light_graph = frame_us(&sample::game_frame_stats, 200, 0.05f);
    double light_free = frame_us(&sample::game_frame_free_stats, 200, 0.05f);
    report_frame("graph 1.0", heavy_graph, 0.0);
    report_frame("free 1.0", heavy_free, heavy_graph);
    report_frame("graph .05", light_graph, 0.0);
    report_frame("free .05", light_free, light_graph);
}
