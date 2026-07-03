#include "systems.h"
#include "parallel.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <tuple>

namespace sample
{

float time_scale = 1.0f;

namespace
{

// `Thread_safe::async` demo instrumentation: concurrent `nav` queries in flight.
std::atomic<int> nav_active{ 0 };
std::atomic<int> nav_peak{ 0 };
// Nav queries that early-outed on the cancel token mid-flight (body-level cancellation).
std::atomic<int> nav_early{ 0 };

// Streaming demo instrumentation: assets processed via `then`, batches via `when_all`.
std::atomic<int> streamed{ 0 };
std::atomic<int> batches{ 0 };

void update_max(std::atomic<int>& max, int value)
{
    int prev = max.load();
    while (value > prev && !max.compare_exchange_weak(prev, value)) {}
}

// Mock a system's CPU cost: spin-wait for the budget. Precise (unlike
// `std::this_thread::sleep_for`, which rounds sub-ms waits up to the ~15 ms Windows
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

// Heavy systems split their per-entity work across chunks. The cost spin is
// divided too, so wall time ~ total_ms / chunks (bounded by free workers).
constexpr int parallel_chunks = 8;

void parallel_fill(Float_store& out, float v, double total_ms)
{
    int n = out.size();
    parallel_for(parallel_chunks, [&out, v, total_ms, n](int c)
    {
        int begin = c * n / parallel_chunks;
        int end = (c + 1) * n / parallel_chunks;
        for (int i = begin; i < end; ++i)
            out.set(i, v);
        spin(total_ms / parallel_chunks);
    });
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

void tick_streaming(ts::Thread_safe<Float_store>& asset_source,
                    const Float_store& input, Float_store& assets)
{
    read_all(input);
    fill(assets, 1.0f);

    // Stream a batch of assets from the read-only source via `Thread_safe::async`,
    // fire-and-forget. Each load is processed by a `then` continuation; a `when_all`
    // over the batch fires a finalize continuation once every load is in. The
    // continuations run on workers (not blocking this node), like the nav demo --
    // the chain stays alive via the loads' pipe jobs.
    auto load = [](const Float_store& src) { spin(0.2); return src.size() > 0 ? src.get(0) : 1.0f; };
    auto process = [](float) { streamed.fetch_add(1, std::memory_order_relaxed); };

    ts::Task<float> a = asset_source.async(load);
    ts::Task<float> b = asset_source.async(load);
    ts::Task<float> c = asset_source.async(load);
    ts::Task<float> d = asset_source.async(load);

    a.then(process);
    b.then(process);
    c.then(process);
    d.then(process);

    ts::when_all(a, b, c, d).then([](std::tuple<float, float, float, float>&)
    {
        batches.fetch_add(1, std::memory_order_relaxed);
    });

    spin(0.5);   // the node's own cost, overlapping the async loads
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

void tick_ai(ts::Thread_safe<Float_store>& nav,
             const Float_store& world_xf_prev, const Float_store& paths,
             const Float_store& game_state, Float_store& intents)
{
    read_all(world_xf_prev);
    read_all(paths);
    read_all(game_state);

    // Per-agent path queries against the read-only `nav` service via the
    // `Thread_safe::async` path: fired concurrently, they run as concurrent
    // readers on `nav`'s pipe (on other workers, overlapping AI's own logic
    // below). Safe because `nav` has no writer this frame -- the legitimate use of
    // async access outside the static graph's declared edges.
    //
    // Fire-and-forget: AI does NOT block on the results. Consuming them would mean
    // blocking inside a graph node (the anti-pattern we avoid -- it ties up a
    // worker and risks deadlock); doing it cleanly needs continuations feeding a
    // downstream node (future work).
    //
    // The queries are speculative: each runs a longer budget than AI needs, and AI
    // cancels the batch once it has done its own work. The query body opts into
    // COOPERATIVE cancellation -- it takes a trailing `Cancellation_token` and polls
    // it inside the budget loop, returning early when AI cancels. That is a body-level
    // early-out (a cooperative return settles the task completed, not cancelled), the
    // opt-in form for cancellation that arrives *while* the body runs.
    ts::Cancellation_source nav_cancel;
    constexpr int queries = 6;
    for (int q = 0; q < queries; ++q)
        nav.async([](const Float_store& n, ts::Cancellation_token tok)
        {
            update_max(nav_peak, nav_active.fetch_add(1) + 1);
            bool bailed = false;
            auto start = std::chrono::steady_clock::now();
            auto budget = std::chrono::duration<double, std::milli>(2.0 * time_scale);
            while (std::chrono::steady_clock::now() - start < budget)
            {
                if (tok.is_cancel_requested()) { bailed = true; break; }   // early-out
                std::this_thread::yield();
            }
            float v = n.size() > 0 ? n.get(0) : 0.0f;
            nav_active.fetch_sub(1);
            if (bailed)
                nav_early.fetch_add(1, std::memory_order_relaxed);
            return v;
        }, nav_cancel.token());

    spin(1.5);   // AI's own logic, overlapping the async nav queries
    nav_cancel.request_cancel();   // AI has its paths -> stop stragglers (they early-out)
    fill(intents, 1.0f);
}

void tick_animation(const Float_store& skeletons, const Float_store& intents, Float_store& local_xf)
{
    read_all(skeletons);
    read_all(intents);
    parallel_fill(local_xf, 2.0f, 3.0);   // internal parallelism: per-skeleton
}

void tick_physics(const Float_store& velocities, const Float_store& game_state, Float_store& bodies)
{
    read_all(velocities);
    read_all(game_state);
    parallel_fill(bodies, 3.0f, 3.0);     // internal parallelism: per-island
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
    parallel_fill(visibility, 1.0f, 1.5);   // internal parallelism: per-view
}

void tick_particles(const Float_store& world_xf, Float_store& particles)
{
    read_all(world_xf);
    parallel_fill(particles, 1.0f, 1.5);    // internal parallelism: per-system
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
    parallel_fill(draw_lists, 1.0f, 2.5);   // internal parallelism: per-pass
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

void reset_stats()
{
    nav_active.store(0);
    nav_peak.store(0);
    nav_early.store(0);
    streamed.store(0);
    batches.store(0);
}

int observed_nav_concurrency()
{
    return nav_peak.load();
}

int nav_early_outs()
{
    return nav_early.load();
}

int assets_streamed()
{
    return streamed.load();
}

int batches_streamed()
{
    return batches.load();
}

} // namespace sample
