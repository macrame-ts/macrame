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

// Touch every element so the access harness fires on each declared store.
void fill(Float_store& s, float v)
{
    for (int i = 0, n = s.size(); i < n; ++i)
        s.set(i, v);
}

void read_all(const Float_store& s)
{
    volatile float sink = 0;
    for (int i = 0, n = s.size(); i < n; ++i)
        sink = s.get(i);
    (void)sink;
}

void copy(const Float_store& from, Float_store& to)
{
    for (int i = 0, n = to.size(); i < n; ++i)
        to.set(i, from.get(i));
}

} // namespace

void tick_input(Float_store& input)
{
    fill(input, 1.0f);
    spin(0.1);
}

void tick_networking(const Float_store& input, Float_store& net)
{
    read_all(input);
    fill(net, 1.0f);
    spin(0.5);
}

void tick_streaming(const Float_store& input, Float_store& assets)
{
    read_all(input);
    fill(assets, 1.0f);
    spin(0.5);
}

void tick_gameplay(const Float_store& world_xf_prev, const Float_store& input,
                   const Float_store& net, Float_store& game_state)
{
    read_all(world_xf_prev);
    read_all(input);
    read_all(net);
    fill(game_state, 1.0f);
    spin(2.0);
}

void tick_navigation(const Float_store& nav, const Float_store& world_xf_prev, Float_store& paths)
{
    read_all(nav);
    read_all(world_xf_prev);
    fill(paths, 1.0f);
    spin(1.0);
}

void tick_ai(const Float_store& world_xf_prev, const Float_store& paths,
             const Float_store& game_state, Float_store& intents)
{
    read_all(world_xf_prev);
    read_all(paths);
    read_all(game_state);
    fill(intents, 1.0f);
    spin(1.5);
}

void tick_animation(const Float_store& skeletons, const Float_store& intents, Float_store& local_xf)
{
    read_all(skeletons);
    read_all(intents);
    fill(local_xf, 2.0f);
    spin(3.0);
}

void tick_physics(const Float_store& velocities, const Float_store& game_state, Float_store& bodies)
{
    read_all(velocities);
    read_all(game_state);
    fill(bodies, 3.0f);
    spin(3.0);
}

void tick_propagation(const Float_store& local_xf, const Float_store& bodies, Float_store& world_xf)
{
    for (int i = 0, n = world_xf.size(); i < n; ++i)
        world_xf.set(i, local_xf.get(i) + bodies.get(i));
    spin(1.0);
}

void tick_cloth(const Float_store& world_xf, Float_store& cloth)
{
    read_all(world_xf);
    fill(cloth, 1.0f);
    spin(1.0);
}

void tick_culling(const Float_store& world_xf, const Float_store& renderables, Float_store& visibility)
{
    read_all(world_xf);
    read_all(renderables);
    fill(visibility, 1.0f);
    spin(1.5);
}

void tick_particles(const Float_store& world_xf, Float_store& particles)
{
    read_all(world_xf);
    fill(particles, 1.0f);
    spin(1.5);
}

void tick_audio(const Float_store& world_xf, Float_store& audio_out)
{
    read_all(world_xf);
    fill(audio_out, 1.0f);
    spin(0.5);
}

void tick_render(const Float_store& world_xf, const Float_store& visibility,
                 const Float_store& particles, Float_store& draw_lists)
{
    read_all(world_xf);
    read_all(visibility);
    read_all(particles);
    fill(draw_lists, 1.0f);
    spin(2.5);
}

void tick_ui(const Float_store& game_state, Float_store& ui)
{
    read_all(game_state);
    fill(ui, 1.0f);
    spin(0.5);
}

void tick_debug(const Float_store& game_state, const Float_store& world_xf)
{
    read_all(game_state);
    read_all(world_xf);
    spin(0.2);
}

void tick_swap(const Float_store& world_xf, Float_store& world_xf_prev)
{
    copy(world_xf, world_xf_prev);
    spin(0.1);
}

double serial_budget_ms()
{
    return 0.1 + 0.5 + 0.5 + 2.0 + 1.0 + 1.5 + 3.0 + 3.0 + 1.0
         + 1.0 + 1.5 + 1.5 + 0.5 + 2.5 + 0.5 + 0.2 + 0.1;
}

} // namespace sample
