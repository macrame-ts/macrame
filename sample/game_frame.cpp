// A mock game-engine frame -- the breadth sample, and a worked optimization
// exercise. ~30 systems over ~35 guarded stores; the whole schedule is derived
// from declared data access. Built in two variants from the SAME system bodies:
//
//   Frame_variant::baseline  -- a straightforward composition. Every system
//     declares what it reads and writes; the graph parallelises it with no
//     hand-tuning. This already runs wide -- the point is that honest
//     single-writer decomposition gets you most of the way for free.
//
//   Frame_variant::optimised -- the same frame after reading its own trace. An
//     `optimise()` section (after construction, before compile) applies the
//     levers the visualization makes obvious: staging the draw producers so they
//     stop serialising, pinning the critical spine inline so unrelated queued
//     work can't wedge between its nodes, and nudging a long off-path node off
//     the critical path. Structural choices that must change a node's access
//     (plain draw writes vs `Deferred` staging) branch at construction on the
//     variant; pure scheduling levers live in `optimise()`.
//
// What the layers show:
//   - `Static_task_graph` -- nodes over guarded stores; every edge derived from
//     parameter const-ness. A real render frame's worth of nodes: a gameplay
//     spine, a split physics pipeline (broadphase -> narrowphase -> solver ->
//     finalize), an animation chain, and a render pipeline that consumes LAST
//     frame's transforms so it overlaps this frame's simulation (the render
//     thread with one frame of latency, modelled with `Versioned` and nothing
//     else).
//   - `Versioned<Transforms>` -- the packaged double-buffer. Simulation stages
//     this frame's transforms and flips; render + audio read the previous
//     version (declared before the flip) and run from t=0; cloth reads the
//     fresh version (declared after).
//   - `Deferred<Draw_lists>` -- the command buffer. In the optimised variant the
//     draw producers stage grant-free and go wide; the baseline writes the queue
//     directly, so the producers serialise -- a real inefficiency the trace
//     shows and the exercise removes.
//   - Dynamic work outside the graph: streaming fires `async` loads joined by
//     `when_all`; AI fires speculative cancellable nav queries.
//   - Internal parallelism: heavy systems split their work with `parallel_for`.
//
// The core logic is plainly thread-unsafe single-threaded code -- no atomics, no
// locks. The library turns it into a safe parallel schedule from the access
// declarations alone; the systems never learn about threading.

#include "ts/access.h"
#include "ts/deferred.h"
#include "ts/guarded.h"
#include "ts/parallel_for.h"
#include "ts/static_task_graph.h"
#include "ts/task.h"
#include "ts/versioned.h"

#if TS_PROFILING
#include "graph_trace.h"   // tools/: the aggregating runtime trace (see trace_game_frame)
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace sample
{

// Baseline = a straightforward composition; optimised = the same frame after the
// levers the trace makes obvious. `build_frame_graph` takes the variant.
enum class Frame_variant { baseline, optimised };

namespace
{

// Scales every system's mock cost. 1.0 = heavy 30 fps-class budgets; tests and
// the stress loop use a small scale so many frames run fast.
float time_scale = 1.0f;

// The variant traces run on this many workers -- heavy enough (~35 ms of work)
// to warrant more than the machine default when starved; retune freely.
constexpr int variant_workers = 6;

// --- instrumentation --------------------------------------------------------------

std::atomic<int> nav_active{ 0 };
std::atomic<int> nav_peak{ 0 };
std::atomic<int> nav_early{ 0 };
std::atomic<int> streamed{ 0 };
std::atomic<int> batches{ 0 };
std::atomic<long long> drawn{ 0 };
std::atomic<int> hud_snapshots{ 0 };

void update_max(std::atomic<int>& max, int value)
{
    int prev = max.load();
    while (value > prev && !max.compare_exchange_weak(prev, value)) {}
}

void reset_stats()
{
    nav_active.store(0);
    nav_peak.store(0);
    nav_early.store(0);
    streamed.store(0);
    batches.store(0);
    drawn.store(0);
    hud_snapshots.store(0);
}

// Mock a system's CPU cost: spin-wait for the budget. Precise (unlike
// `sleep_for`, which rounds sub-ms waits up to the ~15 ms Windows timer tick)
// and, like real compute, it occupies the worker for the duration so graph
// parallelism is measurable.
void spin(double ms)
{
    auto target = std::chrono::duration<double, std::milli>(ms * time_scale);
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < target)
        std::this_thread::yield();
}

// --- the stores -------------------------------------------------------------------

// A component store (one float per entity) -- stands in for an ECS component
// array. Plain single-threaded code; every public method asserts the caller
// declared access, which is all it has to do to participate in the frame graph.
class Float_store
{
public:
    explicit Float_store(int entities) : data_(entities, 0.0f) {}

    int size() const
    {
        TS_CHECK_ACCESS();
        return static_cast<int>(data_.size());
    }

    float get(int i) const
    {
        TS_CHECK_ACCESS();
        return data_[i];
    }

    void set(int i, float v)
    {
        TS_CHECK_ACCESS();
        data_[i] = v;
    }

private:
    std::vector<float> data_;
};

// The published world transforms -- the versioned type. `apply` replaces the
// whole batch (idempotent), so the replay resync re-applies deterministically.
class Transforms
{
public:
    void apply(const std::vector<float>& batch)
    {
        TS_CHECK_ACCESS();
        data_ = batch;
    }

    int size() const
    {
        TS_CHECK_ACCESS();
        return static_cast<int>(data_.size());
    }

    float get(int i) const
    {
        TS_CHECK_ACCESS();
        return data_[i];
    }

private:
    std::vector<float> data_;
};

// The render queue -- the `Deferred` target (optimised) or a plain write target
// (baseline). Producers in the optimised variant stage commands; the submit node
// applies them. In the baseline the producers push directly under a write grant.
class Draw_lists
{
public:
    void push_batch(int commands)
    {
        TS_CHECK_ACCESS();
        pending_ += commands;
    }

    int count() const
    {
        TS_CHECK_ACCESS();
        return pending_;
    }

    void clear()
    {
        TS_CHECK_ACCESS();
        pending_ = 0;
    }

private:
    int pending_ = 0;
};

// Role aliases over the one mock store type: system signatures then say which
// store they touch (`const Net&` instead of a second `const Float_store&`).
using Skeletons = Float_store;
using Nav_mesh = Float_store;
using Renderables = Float_store;
using Asset_source = Float_store;
using Input = Float_store;
using Camera = Float_store;
using Net = Float_store;
using Script_events = Float_store;
using Assets = Float_store;
using Nav_tiles = Float_store;
using Combat = Float_store;
using Economy = Float_store;
using Quests = Float_store;
using Paths = Float_store;
using Intents = Float_store;
using Anim_pose = Float_store;
using Local_xf = Float_store;
using Skin_matrices = Float_store;
using Velocities = Float_store;
using Broad_pairs = Float_store;
using Contacts = Float_store;
using Bodies = Float_store;
using Visibility = Float_store;
using Vis_final = Float_store;
using Shadow_map = Float_store;
using Cloth = Float_store;
using Particles = Float_store;
using Audio_out = Float_store;
using Vfx = Float_store;
using UI = Float_store;
using Replication = Float_store;
using Stats = Float_store;
using GC = Float_store;
using Debug_out = Float_store;

// --- the world --------------------------------------------------------------------

// Every mutable store has a single writer system, so the graph derives a clean
// DAG. Transforms are `Versioned` (readers before the flip see last frame,
// readers after see this frame); the draw queue is `Deferred` (optimised
// producers stage, submit applies) with a plain `Guarded` front the baseline
// producers write directly.
struct World
{
    explicit World(int n)
        : skeletons{ ts::Named{"skeletons"}, n }, nav_mesh{ ts::Named{"nav_mesh"}, n }
        , renderables{ ts::Named{"renderables"}, n }, asset_source{ ts::Named{"asset_source"}, n }
        , input{ ts::Named{"input"}, n }, camera{ ts::Named{"camera"}, n }
        , net{ ts::Named{"net"}, n }, script_events{ ts::Named{"script_events"}, n }
        , assets{ ts::Named{"assets"}, n }, nav_tiles{ ts::Named{"nav_tiles"}, n }
        , combat{ ts::Named{"combat"}, n }, economy{ ts::Named{"economy"}, n }
        , quests{ ts::Named{"quests"}, n }, paths{ ts::Named{"paths"}, n }
        , intents{ ts::Named{"intents"}, n }, anim_pose{ ts::Named{"anim_pose"}, n }
        , local_xf{ ts::Named{"local_xf"}, n }, skin_matrices{ ts::Named{"skin_matrices"}, n }
        , velocities{ ts::Named{"velocities"}, n }, broad_pairs{ ts::Named{"broad_pairs"}, n }
        , contacts{ ts::Named{"contacts"}, n }, bodies{ ts::Named{"bodies"}, n }
        , visibility{ ts::Named{"visibility"}, n }, vis_final{ ts::Named{"vis_final"}, n }
        , shadow_map{ ts::Named{"shadow_map"}, n }, cloth{ ts::Named{"cloth"}, n }
        , particles{ ts::Named{"particles"}, n }, audio_out{ ts::Named{"audio_out"}, n }
        , vfx{ ts::Named{"vfx"}, n }, UI{ ts::Named{"UI"}, n }
        , replication{ ts::Named{"replication"}, n }, stats{ ts::Named{"stats"}, n }
        , gc{ ts::Named{"gc"}, n }, debug_out{ ts::Named{"debug_out"}, n }
    {}

    // read-only static inputs (no system writes them this frame)
    ts::Guarded<Skeletons> skeletons;
    ts::Guarded<Nav_mesh> nav_mesh;
    ts::Guarded<Renderables> renderables;
    ts::Guarded<Asset_source> asset_source;

    // single-writer outputs (writer named by the type)
    ts::Guarded<Input> input;
    ts::Guarded<Camera> camera;
    ts::Guarded<Net> net;
    ts::Guarded<Script_events> script_events;
    ts::Guarded<Assets> assets;
    ts::Guarded<Nav_tiles> nav_tiles;
    ts::Guarded<Combat> combat;
    ts::Guarded<Economy> economy;
    ts::Guarded<Quests> quests;
    ts::Guarded<Paths> paths;
    ts::Guarded<Intents> intents;
    ts::Guarded<Anim_pose> anim_pose;
    ts::Guarded<Local_xf> local_xf;
    ts::Guarded<Skin_matrices> skin_matrices;
    ts::Guarded<Velocities> velocities;
    ts::Guarded<Broad_pairs> broad_pairs;
    ts::Guarded<Contacts> contacts;
    ts::Guarded<Bodies> bodies;
    ts::Guarded<Visibility> visibility;
    ts::Guarded<Vis_final> vis_final;
    ts::Guarded<Shadow_map> shadow_map;

    // the published transforms: staged by propagation, flipped by the publish node
    ts::Versioned<Transforms> transforms{ ts::Named{"transforms"} };

    ts::Guarded<Cloth> cloth;
    ts::Guarded<Particles> particles;
    ts::Guarded<Audio_out> audio_out;
    ts::Guarded<Vfx> vfx;
    ts::Guarded<UI> UI;
    ts::Guarded<Replication> replication;
    ts::Guarded<Stats> stats;
    ts::Guarded<GC> gc;
    ts::Guarded<Debug_out> debug_out;

    // the render queue: baseline producers write it directly, optimised producers
    // stage into `draw_staged` and submit applies the batch
    ts::Guarded<Draw_lists> draw_lists{ ts::Named{"draw_lists"} };
    ts::Deferred<Draw_lists> draw_staged{ draw_lists };
};

// --- shared helpers ---------------------------------------------------------------

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

void read_all(const Transforms& t)
{
    volatile float sink = 0;
    for (int i = 0, n = t.size(); i < n; ++i)
        sink = t.get(i);
    (void)sink;
}

// Internally-parallel systems run their work through `parallel_for`, under the
// calling node's grant (the node holds exclusive access to its output; slices
// are disjoint). The manual slicing is an artifact of MOCKING cost with a spin
// (which can't be divided per trivial item); real per-item work would iterate
// items directly and let the library balance.
constexpr int cost_slices = 8;

void parallel_fill(Float_store& out, float v, double total_ms)
{
    int n = out.size();
    ts::parallel_for(cost_slices, [&out, v, total_ms, n](int c)
    {
        int begin = c * n / cost_slices;
        int end = (c + 1) * n / cost_slices;
        for (int i = begin; i < end; ++i)
            out.set(i, v);
        spin(total_ms / cost_slices);
    });
}

void parallel_cost(double total_ms)
{
    ts::parallel_for(cost_slices, [total_ms](int)
    {
        spin(total_ms / cost_slices);
    });
}

// --- the systems ------------------------------------------------------------------
// Free functions with typed parameters: the const-ness is the access
// declaration the graph reads. Budgets (ms @ scale 1.0) in comments.

// Frame head ----

void tick_input(Input& input)                                                     // 0.1
{
    fill(input, 1.0f);
    spin(0.1);
}

void tick_camera(const Input& input, Camera& camera)                              // 0.1
{
    read_all(input);
    fill(camera, 1.0f);
    spin(0.1);
}

void tick_networking(const Input& input, Net& net)                                // 0.5
{
    read_all(input);
    fill(net, 1.0f);
    spin(0.5);
}

// Gameplay scripting / VM tick: dispatches this frame's gameplay events. A real
// engine burns real time here; the trio reads its output.
void tick_scripting(const Input& input, const Net& net, Script_events& events)    // 0.6
{
    read_all(input);
    read_all(net);
    fill(events, 1.0f);
    spin(0.6);
}

// Streaming: async loads from the read-only source, overlapping the node's own
// decompression cost. Fire-and-forget; handles dropped, chain stays alive.
void tick_streaming(ts::Guarded<Asset_source>& asset_source,
                    const Input& input, Assets& assets)                           // 1.5
{
    read_all(input);
    fill(assets, 1.0f);

    auto load = [](const Asset_source& src) { spin(0.2); return src.size() > 0 ? src.get(0) : 1.0f; };
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

    spin(1.5);
}

// A background nav-mesh tile rebuild: streams and bakes navigation tiles into its
// own store, which navigation reads. An independent writer (fed by input) --
// off the critical path, but its output feeds navigation.
void tick_navmesh_rebuild(const Input& input, Nav_tiles& tiles)                   // 0.6
{
    read_all(input);
    fill(tiles, 1.0f);
    spin(0.6);
}

// Gameplay trio: shares inputs (last frame's transforms, input, net, script
// events), owns disjoint outputs -> runs in parallel.
// Combat runs a threat/damage pass per entity. The baseline runs it serially;
// the optimised variant parallelises it across entities -- the trace shows
// combat as one of the two fattest bars on the critical path, so a per-entity
// split is the obvious cut. `parallel` picks the shape.
void tick_combat(const Transforms& prev_xf, const Input& input, const Net& net,
                 const Script_events& events, Combat& combat, bool parallel)       // 0.8
{
    read_all(prev_xf);
    read_all(input);
    read_all(net);
    read_all(events);
    if (parallel)
        parallel_fill(combat, 1.0f, 0.8);
    else
    {
        fill(combat, 1.0f);
        spin(0.8);
    }
}

void tick_economy(const Transforms& prev_xf, const Input& input, const Net& net,
                  const Script_events& events, Economy& economy)                  // 0.7
{
    read_all(prev_xf);
    read_all(input);
    read_all(net);
    read_all(events);
    fill(economy, 1.0f);
    spin(0.7);
}

void tick_quests(const Transforms& prev_xf, const Input& input, const Net& net,
                 const Script_events& events, Quests& quests)                     // 0.7
{
    read_all(prev_xf);
    read_all(input);
    read_all(net);
    read_all(events);
    fill(quests, 1.0f);
    spin(0.7);
}

void tick_navigation(const Nav_mesh& nav_mesh, const Nav_tiles& tiles,
                     const Transforms& prev_xf, Paths& paths)                     // 2.5
{
    read_all(nav_mesh);
    read_all(tiles);
    read_all(prev_xf);
    parallel_fill(paths, 1.0f, 2.5);
}

// AI: per-agent path queries against the read-only nav service via async --
// concurrent readers on other workers, overlapping AI's own logic. Speculative:
// each query runs a longer budget than AI needs and AI cancels the batch once
// done (cooperative early-out via the trailing token). AI reads the trio outputs
// (this frame in the baseline; the optimised variant is discussed in `optimise`).
void tick_AI(ts::Guarded<Nav_mesh>& nav_service,
             const Transforms& prev_xf, const Paths& paths, const Combat& combat,
             const Economy& economy, const Quests& quests, Intents& intents)      // 1.5
{
    read_all(prev_xf);
    read_all(paths);
    read_all(combat);
    read_all(economy);
    read_all(quests);

    ts::Cancellation_source nav_cancel;
    constexpr int queries = 6;
    for (int q = 0; q < queries; ++q)
        nav_service.async([](const Nav_mesh& n, ts::Cancellation_token tok)
        {
            update_max(nav_peak, nav_active.fetch_add(1) + 1);
            bool bailed = false;
            auto start = std::chrono::steady_clock::now();
            auto budget = std::chrono::duration<double, std::milli>(2.0 * time_scale);
            while (std::chrono::steady_clock::now() - start < budget)
            {
                if (tok.is_cancel_requested()) { bailed = true; break; }
                std::this_thread::yield();
            }
            float v = n.size() > 0 ? n.get(0) : 0.0f;
            nav_active.fetch_sub(1);
            if (bailed)
                nav_early.fetch_add(1, std::memory_order_relaxed);
            return v;
        }, { .token = nav_cancel.token() });

    parallel_cost(1.5);
    nav_cancel.request_cancel();
    fill(intents, 1.0f);
}

// Animation chain: graph eval -> IK/post -> skinning.
void tick_anim_graph(const Skeletons& skeletons, const Intents& intents,
                     Anim_pose& anim_pose)                                        // 2.5
{
    read_all(skeletons);
    read_all(intents);
    parallel_fill(anim_pose, 1.0f, 2.5);
}

// IK / post: refines the pose into the final local transforms. IK is per
// character -- embarrassingly parallel across characters -- so the baseline's
// serial pass is a critical-path bar the optimised variant splits. Writes
// local_xf = 2.0 (half of the deterministic transform output).
void tick_ik_post(const Anim_pose& anim_pose, Local_xf& local_xf, bool parallel)  // 0.8
{
    read_all(anim_pose);
    if (parallel)
        parallel_fill(local_xf, 2.0f, 0.8);
    else
    {
        fill(local_xf, 2.0f);
        spin(0.8);
    }
}

// Skinning: matrix palette from the final pose, consumed by next frame's render.
void tick_skinning(const Local_xf& local_xf, Skin_matrices& skin)                 // 1.0
{
    read_all(local_xf);
    parallel_fill(skin, 1.0f, 1.0);
}

// Physics pipeline: broadphase -> narrowphase -> solver -> finalize. Broadphase
// reads body positions (last frame's, before finalize rewrites them this frame);
// the solver integrates velocities; finalize integrates positions. Finalize
// writes bodies = 3.0 (the other half of the deterministic transform output).
void tick_broadphase(const Bodies& bodies, Broad_pairs& pairs)                    // 0.8
{
    read_all(bodies);
    parallel_fill(pairs, 1.0f, 0.8);
}

void tick_narrowphase(const Broad_pairs& pairs, Contacts& contacts)              // 1.5
{
    read_all(pairs);
    parallel_fill(contacts, 1.0f, 1.5);
}

void tick_solver(const Contacts& contacts, const Combat& combat,
                 Velocities& velocities)                                          // 2.8
{
    read_all(contacts);
    read_all(combat);
    parallel_fill(velocities, 1.0f, 2.8);
}

void tick_finalize(const Velocities& velocities, Bodies& bodies)                  // 0.4
{
    read_all(velocities);
    parallel_fill(bodies, 3.0f, 0.4);
}

// Render pipeline: consumes LAST frame's transforms (declared before the flip),
// so it overlaps this frame's simulation -- the render thread with one frame of
// latency. Its own working stores (visibility, shadows) are this-frame.
void tick_frustum_cull(const Transforms& prev_xf, const Camera& camera,
                       const Renderables& renderables, Visibility& visibility)    // 0.9
{
    read_all(prev_xf);
    read_all(camera);
    read_all(renderables);
    parallel_fill(visibility, 1.0f, 0.9);
}

void tick_occlusion_cull(const Transforms& prev_xf, const Visibility& visibility,
                         Vis_final& vis_final)                                    // 0.9
{
    read_all(prev_xf);
    read_all(visibility);
    parallel_fill(vis_final, 1.0f, 0.9);
}

void tick_shadow(const Transforms& prev_xf, const Skeletons& skeletons,
                 Shadow_map& shadow_map)                                          // 1.0
{
    read_all(prev_xf);
    read_all(skeletons);
    parallel_fill(shadow_map, 1.0f, 1.0);
}

// Post-flip readers of the FRESH version. Cloth simulates on this frame's
// transforms.
void tick_cloth(const Transforms& xf, Cloth& cloth)                               // 2.0
{
    read_all(xf);
    parallel_fill(cloth, 1.0f, 2.0);
}

// Audio mixes off LAST frame's transforms (declared before the flip): one frame
// of positional latency is inaudible, and the serial mixer runs from t=0.
void tick_audio(const Transforms& prev_xf, Audio_out& audio_out)                  // 1.6
{
    read_all(prev_xf);
    fill(audio_out, 1.0f);
    spin(1.6);   // deliberately serial: a single-threaded mixer is realistic
}

void tick_vfx(const Transforms& prev_xf, const Particles& particles, Vfx& vfx)    // 0.6
{
    read_all(prev_xf);
    read_all(particles);
    fill(vfx, 1.0f);
    spin(0.6);
}

// Replication snapshot: reads the gameplay + AI outputs, packs a network
// snapshot. Terminal, multi-read.
void tick_replication(const Combat& combat, const Economy& economy,
                      const Quests& quests, const Intents& intents, Replication& rep)  // 0.4
{
    read_all(combat);
    read_all(economy);
    read_all(quests);
    read_all(intents);
    fill(rep, 1.0f);
    spin(0.4);
}

// End-of-frame stats: reads across many systems for the telemetry overlay.
// Terminal.
void tick_stats(const Combat& combat, const Economy& economy, const Bodies& bodies,
                const Visibility& visibility, Stats& stats)                       // 0.3
{
    read_all(combat);
    read_all(economy);
    read_all(bodies);
    read_all(visibility);
    fill(stats, 1.0f);
    spin(0.3);
}

// Memory / GC tick: independent background bookkeeping.
void tick_gc(GC& gc)                                                              // 1.0
{
    parallel_fill(gc, 1.0f, 1.0);
}

// --- the frame graph --------------------------------------------------------------

double serial_budget_ms()
{
    return 0.1 + 0.1 + 0.5 + 0.6 + 1.5 + 0.6           // head + streaming + rebuild
         + 0.8 + 0.7 + 0.7                              // trio
         + 2.5 + 1.5                                    // nav + AI
         + 2.5 + 0.8 + 1.0                              // anim chain
         + 0.8 + 1.5 + 2.8 + 0.4                        // physics
         + 1.0                                          // propagation
         + 0.9 + 0.9 + 1.0 + 3.0 + 0.8                  // render (frustum,occl,shadow,cmd,submit)
         + 2.0 + 1.6 + 0.6                              // cloth, audio, vfx
         + 2.0 + 1.5                                    // particles, UI (draw producers)
         + 0.4 + 0.3 + 1.0 + 0.2;                       // replication, stats, gc, debug
}

// Draw producers: the two shapes differ in access, so they branch on the variant
// at construction. Baseline writes `draw_lists` directly (a write grant -> the
// producers serialise on the queue); optimised stages into `draw_staged`
// grant-free (-> they go wide, submit applies the batch).

// Command recording: the heavy render producer (batches draw calls from the
// visible set + shadows).
ts::Graph_node add_cmd_record(ts::Static_task_graph& g, World& w, Frame_variant v)  // 3.0
{
    if (v == Frame_variant::baseline)
        return g.add_node("cmd_record",
            [](const Vis_final& vis, const Shadow_map& shadows, const Renderables& r,
               Draw_lists& draws)
            {
                read_all(vis);
                read_all(shadows);
                read_all(r);
                parallel_cost(3.0);
                draws.push_batch(vis.size());
            }, w.vis_final, w.shadow_map, w.renderables, w.draw_lists);

    return g.add_node("cmd_record",
        [rec = w.draw_staged.recorder()](const Vis_final& vis, const Shadow_map& shadows,
                                         const Renderables& r) mutable
        {
            read_all(vis);
            read_all(shadows);
            read_all(r);
            parallel_cost(3.0);
            rec.stage([n = vis.size()](Draw_lists& d) { d.push_batch(n); });
        }, w.vis_final, w.shadow_map, w.renderables);
}

ts::Graph_node add_particles(ts::Static_task_graph& g, World& w, Frame_variant v)   // 2.0
{
    if (v == Frame_variant::baseline)
        return g.add_node("particles",
            [](const Transforms& prev_xf, Particles& p, Draw_lists& draws)
            {
                read_all(prev_xf);
                parallel_fill(p, 1.0f, 2.0);
                draws.push_batch(p.size() / 4);
            }, w.transforms.state(), w.particles, w.draw_lists);

    return g.add_node("particles",
        [rec = w.draw_staged.recorder()](const Transforms& prev_xf, Particles& p) mutable
        {
            read_all(prev_xf);
            parallel_fill(p, 1.0f, 2.0);
            rec.stage([n = p.size() / 4](Draw_lists& d) { d.push_batch(n); });
        }, w.transforms.state(), w.particles);
}

ts::Graph_node add_UI(ts::Static_task_graph& g, World& w, Frame_variant v)          // 1.5
{
    // Baseline: writes the queue directly (serialises with the other producers)
    // and lays out the UI serially -- a 1.5 ms bar the trace flags as critical
    // (submit waits on it). Optimised: stages grant-free AND lays out widgets in
    // parallel.
    if (v == Frame_variant::baseline)
        return g.add_node("UI",
            [](const Quests& quests, UI& u, Draw_lists& draws)
            {
                read_all(quests);
                fill(u, 1.0f);
                spin(1.5);
                draws.push_batch(u.size() / 10);
            }, w.quests, w.UI, w.draw_lists);

    return g.add_node("UI",
        [rec = w.draw_staged.recorder()](const Quests& quests, UI& u) mutable
        {
            read_all(quests);
            parallel_fill(u, 1.0f, 1.5);
            rec.stage([n = u.size() / 10](Draw_lists& d) { d.push_batch(n); });
        }, w.quests, w.UI);
}

// Submit: consumes the draw queue. Baseline just reads the directly-written
// counts; optimised commits the staged batch under its own grant.
ts::Graph_node add_submit(ts::Static_task_graph& g, World& w, Frame_variant v)      // 0.8
{
    if (v == Frame_variant::baseline)
        return g.add_node("submit",
            [](const Transforms& prev_xf, Draw_lists& dl)
            {
                read_all(prev_xf);
                drawn.fetch_add(dl.count(), std::memory_order_relaxed);
                parallel_cost(0.8);
                dl.clear();
            }, w.transforms.state(), w.draw_lists);

    return g.add_node("submit",
        [&w](const Transforms& prev_xf, Draw_lists& dl)
        {
            read_all(prev_xf);
            w.draw_staged.commit(dl);
            drawn.fetch_add(dl.count(), std::memory_order_relaxed);
            parallel_cost(0.8);
            dl.clear();
        }, w.transforms.state(), w.draw_lists);
}

// Build the frame. Construction is identical for both variants except the draw
// producers' access shape (baseline write / optimised stage), handled in the
// add_* helpers above. The optimised variant then runs `optimise()` -- pure
// scheduling levers (edges, inline) applied after construction, before compile.
ts::Static_task_graph build_frame_graph(World& w, Frame_variant variant,
                                        const char* DOT_path = nullptr)
{
    ts::Static_task_graph g;

    // Priorities model importance, not measured wins, and are IDENTICAL in both
    // variants -- graph compilation ignores them (they only order ready tasks in
    // the scheduler's queues), so they are kept for their visualization, not used
    // as an optimization lever. `low` on the deferrable terminal leaves, `high`
    // on the longest pole and the present deadline.

    // Frame head.
    auto input = g.add_node("input", &tick_input, w.input);
    g.add_node("camera", &tick_camera, w.input, w.camera);
    g.add_node("networking", &tick_networking, w.input, w.net);
    auto scripting = g.add_node("scripting", &tick_scripting, w.input, w.net, w.script_events);
    g.add_node("streaming", [&w](const Input& in, Assets& assets)
    {
        tick_streaming(w.asset_source, in, assets);
    }, w.input, w.assets);
    g.add_node("navmesh_rebuild", &tick_navmesh_rebuild, w.input, w.nav_tiles);

    // Gameplay trio. Combat is parallelised in the optimised variant (a critical
    // bar); economy/quests stay serial (off the critical path -- no point).
    const bool opt = variant == Frame_variant::optimised;
    auto combat = g.add_node("combat",
        [opt](const Transforms& prev_xf, const Input& in, const Net& net,
              const Script_events& ev, Combat& c)
        {
            tick_combat(prev_xf, in, net, ev, c, opt);
        }, w.transforms.state(), w.input, w.net, w.script_events, w.combat);
    g.add_node("economy", &tick_economy,
        w.transforms.state(), w.input, w.net, w.script_events, w.economy);
    g.add_node("quests", &tick_quests,
        w.transforms.state(), w.input, w.net, w.script_events, w.quests);

    // AI.
    g.add_node("navigation", &tick_navigation, w.nav_mesh, w.nav_tiles, w.transforms.state(), w.paths);
    auto ai = g.add_node("AI", [&w](const Transforms& prev_xf, const Paths& paths,
                                    const Combat& c, const Economy& e, const Quests& q,
                                    Intents& intents)
    {
        tick_AI(w.nav_mesh, prev_xf, paths, c, e, q, intents);
    }, w.transforms.state(), w.paths, w.combat, w.economy, w.quests, w.intents);

    // Animation chain.
    auto anim_graph = g.add_node("anim_graph", &tick_anim_graph, w.skeletons, w.intents, w.anim_pose);
    auto ik_post = g.add_node("ik_post",
        [opt](const Anim_pose& pose, Local_xf& lx) { tick_ik_post(pose, lx, opt); },
        w.anim_pose, w.local_xf);
    g.add_node("skinning", &tick_skinning, w.local_xf, w.skin_matrices);

    // Physics pipeline.
    g.add_node("broadphase", &tick_broadphase, w.bodies, w.broad_pairs);
    g.add_node("narrowphase", &tick_narrowphase, w.broad_pairs, w.contacts);
    g.add_node("solver", &tick_solver, w.contacts, w.combat, w.velocities)
        .priority(ts::Priority::high);
    g.add_node("finalize", &tick_finalize, w.velocities, w.bodies);

    // Propagation: this frame's transforms from animation + physics, staged
    // grant-free (velocity-aware interpolation reads velocities too, so the
    // physics->propagation edge derives from two conflicts).
    auto propagation = g.add_node("propagation",
        [rec = w.transforms.recorder()](const Local_xf& local_xf, const Bodies& bodies,
                                        const Velocities& velocities) mutable
        {
            read_all(velocities);
            std::vector<float> out(static_cast<std::size_t>(local_xf.size()));
            for (int i = 0, n = local_xf.size(); i < n; ++i)
                out[static_cast<std::size_t>(i)] = local_xf.get(i) + bodies.get(i);
            parallel_cost(1.0);
            rec.stage([batch = std::move(out)](Transforms& t) { t.apply(batch); });
        },
        w.local_xf, w.bodies, w.velocities);

    // Render pipeline -- declared BEFORE the flip, so it reads LAST frame's
    // transforms and overlaps this frame's simulation.
    g.add_node("frustum_cull", &tick_frustum_cull,
        w.transforms.state(), w.camera, w.renderables, w.visibility);
    g.add_node("occlusion_cull", &tick_occlusion_cull,
        w.transforms.state(), w.visibility, w.vis_final);
    g.add_node("shadow", &tick_shadow, w.transforms.state(), w.skeletons, w.shadow_map);
    auto cmd_record = add_cmd_record(g, w, variant);
    auto particles = add_particles(g, w, variant);
    auto UI_node = add_UI(g, w, variant);
    auto submit = add_submit(g, w, variant);
    submit.priority(ts::Priority::high);
    // Baseline: submit reads the queue the producers wrote -> conflict edges order
    // it after them. Optimised: staging holds no grant on the queue, so the
    // ordering is intent, declared explicitly.
    if (variant == Frame_variant::optimised)
        submit.after(cmd_record).after(particles).after(UI_node);

    // Off-path leaves.
    g.add_node("audio", &tick_audio, w.transforms.state(), w.audio_out)
        .priority(ts::Priority::low);
    g.add_node("vfx", &tick_vfx, w.transforms.state(), w.particles, w.vfx);
    g.add_node("replication", &tick_replication,
        w.combat, w.economy, w.quests, w.intents, w.replication);
    g.add_node("stats", &tick_stats, w.combat, w.economy, w.bodies, w.visibility, w.stats);
    g.add_node("gc", &tick_gc, w.gc);
    g.add_node("debug_overlay", [](const auto& economy, const auto& xf)   // 0.2
    {
        read_all(economy);
        read_all(xf);
        spin(0.2);
    }, w.economy, w.transforms.state()).priority(ts::Priority::low);

    // Cloth: a version-choice lever. The baseline reads the FRESH transforms
    // (post-flip), so it sits alone on the tail -- the last node in the frame.
    // The optimised variant reads LAST frame's transforms (declared before the
    // flip, like audio): one frame of cloth latency is invisible, and the node
    // runs alongside the (now short) spine instead of after it. This only pays
    // once the spine has been shortened enough to leave idle capacity for cloth
    // to fill -- on the un-optimised spine the same move was neutral (cloth just
    // competed with the critical chain). Structural (the version is set by
    // declaration order vs the flip), so it branches here.
    ts::Graph_node cloth;
    if (variant == Frame_variant::optimised)
        cloth = g.add_node("cloth", &tick_cloth, w.transforms.state(), w.cloth);

    auto flip = g.add_node("flip", ts::publish_body(w.transforms), w.transforms.state());
    flip.after(propagation);

    if (variant == Frame_variant::baseline)
    {
        cloth = g.add_node("cloth", &tick_cloth, w.transforms.state(), w.cloth);
        cloth.after(flip);
    }

    // --- the optimisation section -----------------------------------------------
    // The optimised variant's levers, applied after construction. The two that
    // move the makespan are the per-node splits (combat, ik_post -- branched in
    // their bodies above), because the frame is critical-path bound: the trace
    // shows the makespan set by the serial sim spine, so ONLY shortening that
    // chain helps. The draw-staging lever (Deferred, in the add_* helpers)
    // improves utilization but not makespan -- it frees cores the critical path
    // wasn't using. Pure scheduling levers (edges/inline) cannot shorten a
    // genuine dependency chain and are deliberately absent here.
    (void)input; (void)scripting; (void)combat; (void)ai;
    (void)anim_graph; (void)ik_post; (void)propagation;

    g.compile(DOT_path);
    return g;
}

} // namespace

// --- entry points -----------------------------------------------------------------

// Run `frames` frames at `scale`; returns the measured average ms/frame, the
// serial budget at that scale, and entity 0's published transform (deterministic:
// propagation wrote local_xf + bodies = 2 + 3 = 5).
void game_frame_stats(int frames, float scale,
                      double& avg_ms, double& serial_ms, float& transform0)
{
    constexpr int entities = 1000;
    time_scale = scale;
    reset_stats();

    World world{ entities };
    ts::Static_task_graph graph = build_frame_graph(world, Frame_variant::baseline);

    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    for (int f = 0; f < frames; ++f)
    {
        // Multi-object access demo: one HUD snapshot reads two stores in one
        // atomically-acquired call -- queues behind whichever node holds a store
        // rather than racing it. Fire-and-forget; World destruction drains pipes.
        ts::async([](const Combat& combat, const Economy& economy)
        {
            if (combat.size() > 0 && economy.size() > 0)
                hud_snapshots.fetch_add(1, std::memory_order_relaxed);
        }, world.combat, world.economy);

        graph.execute().sync();
    }
    double total_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();

    avg_ms = total_ms / frames;
    serial_ms = serial_budget_ms() * scale;
    transform0 = world.transforms.read([](const Transforms& t)
    {
        return t.size() > 0 ? t.get(0) : 0.0f;
    }).sync();
}

// Compile the frame graph and write its structure as Graphviz DOT (no frames run).
void dump_game_frame_dot(const char* path)
{
    World world{ 8 };
    build_frame_graph(world, Frame_variant::baseline, path);
}

namespace
{

// The SVG path for a described run: the description, spaces stripped, becomes a
// suffix before the extension ("baseline" -> "..._baseline.svg").
std::string described_SVG_path(const char* base, const char* description)
{
    std::string path = base;
    std::string suffix;
    for (const char* p = description; *p; ++p)
        if (*p != ' ')
            suffix += *p;
    if (!suffix.empty())
    {
        size_t dot = path.rfind('.');
        path.insert(dot == std::string::npos ? path.size() : dot, "_" + suffix);
    }
    return path;
}

// One traced variant on `scheduler`: build the variant's graph on a fresh World,
// attach a fresh trace, run, write the described SVG.
void trace_variant(int frames, ts::Scheduler& scheduler, Frame_variant variant,
                   const char* base_SVG_path, const char* description, const char* DOT_path)
{
    constexpr int entities = 1000;
    time_scale = 1.0f;
    reset_stats();

    World world{ entities };
    ts::Static_task_graph graph = build_frame_graph(world, variant, DOT_path);

#if TS_PROFILING
    ts::tools::Graph_trace trace;
    std::string title = "Sample \"game_frame\"";
    if (*description)
        title += std::string(" (") + description + ")";
    trace.set_title(std::move(title));
    graph.set_trace(&trace);
    for (int f = 0; f < frames; ++f)
        graph.execute(scheduler).sync();
    graph.set_trace(nullptr);
    std::string path = described_SVG_path(base_SVG_path, description);
    trace.write_SVG(path.c_str());
    std::printf("[game_frame] %s: traced %lld runs -> %s\n",
        description, trace.run_count(), path.c_str());
#else
    for (int f = 0; f < frames; ++f)
        graph.execute(scheduler).sync();
    (void)base_SVG_path; (void)description;
    std::printf("[game_frame] TS_PROFILING is 0: ran %d frames\n", frames);
#endif
}

} // namespace

// Run `frames` frames with an aggregating `Graph_trace` attached, for both
// variants on a dedicated `variant_workers`-worker scheduler: the baseline
// (straightforward composition) and the optimised (same frame, tuned from its
// trace). Writes one average-run SVG per variant plus the structure DOT.
void trace_game_frame(int frames, const char* DOT_path, const char* SVG_path)
{
    ts::Scheduler workers{ { .num_threads = static_cast<uint32_t>(variant_workers) } };
    trace_variant(frames, workers, Frame_variant::baseline, SVG_path, "baseline", DOT_path);
    trace_variant(frames, workers, Frame_variant::optimised, SVG_path, "optimised", nullptr);
}

void run_game_frame_sample(int frames, float scale)
{
    double avg_ms = 0.0, serial_ms = 0.0;
    float transform0 = 0.0f;
    game_frame_stats(frames, scale, avg_ms, serial_ms, transform0);

    int workers = ts::default_scheduler().worker_count();
    double ideal_ms = serial_ms / workers;
    std::printf("\n[game_frame] %d frames, 1000 entities, scale %.2f\n", frames, scale);
    std::printf("  %.2f ms/frame; %.2f ms of work across %d workers -> ideal %.2f ms/frame, "
                "%.0f%% utilization\n",
        avg_ms, serial_ms, workers, ideal_ms, 100.0 * ideal_ms / avg_ms);
    std::printf("  peak %d concurrent nav queries, %d early-outed on cancel\n",
        nav_peak.load(), nav_early.load());
    std::printf("  streamed %d assets via then, %d batches via when_all\n",
        streamed.load(), batches.load());
    std::printf("  %lld draw commands staged/written by producers, applied by submit\n",
        drawn.load());
    std::printf("  %d HUD snapshots via multi-object ts::async (combat + economy)\n",
        hud_snapshots.load());
}

} // namespace sample
