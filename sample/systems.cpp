#include "systems.h"

#include <chrono>
#include <thread>

namespace sample
{

float time_scale = 1.0f;

namespace
{

// Mock a system's CPU cost: spin-wait for the budget. Precise (unlike
// std::this_thread::sleep_for, which rounds sub-ms waits up to the ~15 ms Windows
// timer tick) and, like real compute, it occupies the worker for the duration so
// graph parallelism is measurable. (I/O-bound systems will use a real sleep later.)
void spin(double ms)
{
    auto target = std::chrono::duration<double, std::milli>(ms * time_scale);
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < target)
        std::this_thread::yield();
}

// Touch every element so the access harness actually fires on each store.
void fill(Float_store& s, float v)
{
    for (int i = 0, n = s.size(); i < n; ++i)
        s.set(i, v);
}

void combine(const Float_store& a, const Float_store& b, Float_store& out)
{
    for (int i = 0, n = out.size(); i < n; ++i)
        out.set(i, a.get(i) + b.get(i));
}

void read_all(const Float_store& s)
{
    volatile float sink = 0;
    for (int i = 0, n = s.size(); i < n; ++i)
        sink = s.get(i);
    (void)sink;
}

} // namespace

void tick_input(Float_store& velocities)      { fill(velocities, 1.0f); spin(0.2); }
void tick_animation(const Float_store& skeletons, Float_store& local_xf)
{
    read_all(skeletons);
    fill(local_xf, 2.0f);
    spin(3.0);
}
void tick_physics(const Float_store& velocities, Float_store& bodies)
{
    read_all(velocities);
    fill(bodies, 3.0f);
    spin(3.0);
}
void tick_propagation(const Float_store& local_xf, const Float_store& bodies, Float_store& world_xf)
{
    combine(local_xf, bodies, world_xf);
    spin(1.0);
}
void tick_render(const Float_store& world_xf)  { read_all(world_xf); spin(2.5); }

double serial_budget_ms()
{
    return 0.2 + 3.0 + 3.0 + 1.0 + 2.5;
}

} // namespace sample
