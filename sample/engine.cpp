#include "engine.h"
#include "frame.h"
#include "systems.h"
#include "world.h"

#include <chrono>
#include <cstdio>

namespace sample
{

void run_sample(int frames, float scale)
{
    constexpr int entities = 1000;
    time_scale = scale;

    World world{ entities };
    ts::Static_task_graph graph = build_frame_graph(world);
    reset_stats();

    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    for (int f = 0; f < frames; ++f)
        graph.execute().get();
    double total_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();

    double serial = serial_budget_ms() * scale;
    double per_frame = total_ms / frames;
    std::printf("\n[sample] %d systems, %d frames, %d entities, scale %.2f\n",
        graph.node_count(), frames, entities, scale);
    std::printf("  %.2f ms/frame  (serial budget %.2f ms -> %.2fx speedup)\n",
        per_frame, serial, serial / per_frame);
    std::printf("  peak %d concurrent nav queries (Thread_safe::async during AI)\n",
        observed_nav_concurrency());
}

} // namespace sample
