#include "engine.h"
#include "frame.h"
#include "systems.h"
#include "world.h"

#include <chrono>
#include <cstdio>

namespace sample
{

Frame_stats run_frames(int frames, float scale)
{
    constexpr int entities = 1000;
    time_scale = scale;
    reset_stats();

    World world{ entities };
    ts::Static_task_graph graph = build_frame_graph(world);

    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    for (int f = 0; f < frames; ++f)
        graph.execute().get();
    double total_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();

    // A deterministic frame output: propagation wrote world_xf = local_xf + bodies.
    float xf = world.world_xf.async([](const Float_store& s)
    {
        return s.size() > 0 ? s.get(0) : 0.0f;
    }).get();

    return { frames, total_ms / frames, serial_budget_ms() * scale, xf };
}

void run_sample(int frames, float scale)
{
    Frame_stats s = run_frames(frames, scale);

    std::printf("\n[sample] %d frames, %d entities, scale %.2f\n", s.frames, 1000, scale);
    std::printf("  %.2f ms/frame  (serial budget %.2f ms -> %.2fx speedup)\n",
        s.avg_ms, s.serial_ms, s.serial_ms / s.avg_ms);
    std::printf("  peak %d concurrent nav queries (Thread_safe::async during AI), %d early-outed on cancel\n",
        observed_nav_concurrency(), nav_early_outs());
    std::printf("  streamed %d assets via then, %d batches via when_all\n",
        assets_streamed(), batches_streamed());
}

} // namespace sample
