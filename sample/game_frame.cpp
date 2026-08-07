// A mock game-engine frame -- the breadth sample, and a worked optimization
// exercise. ~30 systems over ~35 guarded stores; the whole schedule is derived
// from declared data access. Built in two variants from the SAME system bodies:
//
//   Frame_variant::baseline  -- a straightforward composition. Every system
//     declares what it reads and writes; the graph parallelises it with no
//     hand-tuning. This already runs wide -- the point is that honest
//     single-writer decomposition gets you most of the way for free.
//
//   Frame_variant::optimised -- the same frame after reading its own trace. The
//     frame is critical-path bound: only shortening the serial sim spine moves
//     the makespan, and pure scheduling levers (edges, inline) cannot shorten a
//     genuine dependency chain. The levers, tagged L1-L4 at their branch points:
//       L1  AI reads LAST frame's gameplay snapshot (`Versioned`) -- deletes the
//           trio->AI edges, the critical-path cut
//       L2  parallelise the fattest critical bars (combat, ik_post, UI layout)
//           -- body-level splits
//       L3  `Deferred` draw staging -- producers stage grant-free and go wide;
//           submit commits (utilization, not makespan)
//       L4  cloth reads LAST frame's transforms -- off the post-flip tail, fills
//           cores once the spine is short
//     Choices that change a node's access (L1, L3, L4) branch at construction on
//     `opt`; the body-level splits (L2) branch inside the system bodies.
//
//   Frame_variant::graph_free -- the baseline frame with no `Static_task_graph` at
//     all: the same systems as multi-object `ts::async` calls, the same schedule
//     written out with `co_await` (`run_frame_graph_free`). The measurement behind
//     "what does the static graph buy me" -- the pipe still gives safety, the graph
//     gives the schedule.
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
//     fresh version (declared after). A second `Versioned` (the gameplay
//     snapshot) is the optimised variant's biggest lever: AI reads last frame's
//     trio results, so the trio->AI edge leaves the critical path.
//   - `Deferred<Draw_lists>` -- the command buffer. In the optimised variant the
//     draw producers stage grant-free and go wide; the baseline writes the queue
//     directly, so the producers serialise -- a real inefficiency the trace
//     shows and the exercise removes.
//   - Dynamic work outside the graph: streaming fires `async` loads a coroutine
//     awaits and folds; AI fires speculative cancellable nav queries.
//   - Internal parallelism: heavy systems split their work with `parallel_for`.
//
// The core logic is plainly thread-unsafe single-threaded code -- no atomics, no
// locks. The library turns it into a safe parallel schedule from the access
// declarations alone; the systems never learn about threading.

#include "ts/access.h"
#include "ts/deferred.h"
#include "ts/guarded.h"
#include "ts/parallel_for.h"
#include "ts/coroutine_support.h"
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
// levers the trace makes obvious. `build_frame_graph` takes either. `graph_free` is
// not a graph at all -- the same baseline frame hand-composed with coroutines and the
// access verbs (`run_frame_graph_free`), the comparison case for what `compile()` buys.
enum class Frame_variant { baseline, optimised, graph_free };

namespace
{

// Scales every system's mock cost. 1.0 = heavy 30 fps-class budgets; tests and
// the stress loop use a small scale so many frames run fast.
float time_scale = 1.0f;

// The variant traces run on this many workers -- heavy enough (~35 ms of work)
// to warrant more than the machine default when starved; retune freely.
constexpr int variant_workers = 6;

// Instrumentation counters, reported by the entry points (declared up here
// because the submit node's body records `drawn`).
std::atomic<int> nav_active{ 0 };
std::atomic<int> nav_peak{ 0 };
std::atomic<int> nav_early{ 0 };
std::atomic<int> streamed{ 0 };
std::atomic<int> batches{ 0 };
std::atomic<long long> drawn{ 0 };
std::atomic<int> hud_snapshots{ 0 };

// --- the stores -------------------------------------------------------------------

// A component store (one float per entity) -- stands in for an ECS component
// array. Plain single-threaded code; every public method asserts the caller
// declared access, which is all it has to do to participate in the frame graph.
class Float_store
{
public:
    explicit Float_store(int entities) : data_(entities, 0.0f) {}

    int size() const { TS_CHECK_ACCESS(); return static_cast<int>(data_.size()); }
    float get(int i) const { TS_CHECK_ACCESS(); return data_[i]; }
    void set(int i, float v) { TS_CHECK_ACCESS(); data_[i] = v; }

private:
    std::vector<float> data_;
};

// The published world transforms -- the versioned type. `apply` replaces the
// whole batch (idempotent), so the replay resync re-applies deterministically.
class Transforms
{
public:
    void apply(const std::vector<float>& batch) { TS_CHECK_ACCESS(); data_ = batch; }
    int size() const { TS_CHECK_ACCESS(); return static_cast<int>(data_.size()); }
    float get(int i) const { TS_CHECK_ACCESS(); return data_[i]; }

private:
    std::vector<float> data_;
};

// The gameplay snapshot: the same default-constructible float-batch shape as
// `Transforms`, double-buffered via `Versioned`. In the optimised variant the
// trio's results are packed into it and published for NEXT frame's AI to read --
// one-frame-latent AI perception, a standard engine pattern (a blackboard / AI
// world snapshot updated at frame end). Reusing `Transforms` also reuses its
// `read_all` overload; the store is named "gameplay" for the trace.
using Gameplay = Transforms;

// The render queue -- the `Deferred` target (optimised) or a plain write target
// (baseline). Producers in the optimised variant stage commands; the submit node
// applies them. In the baseline the producers push directly under a write grant.
class Draw_lists
{
public:
    void push_batch(int commands) { TS_CHECK_ACCESS(); pending_ += commands; }
    int count() const { TS_CHECK_ACCESS(); return pending_; }
    void clear() { TS_CHECK_ACCESS(); pending_ = 0; }

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
// An aggregate: construct as `World world{ entities };`. `entity_count` is the sole
// initialised member; every store self-initialises from it via NSDMI (which is why
// the count must be declared first) and self-names for the trace.
struct World
{
    int entity_count;

    // read-only static inputs (no system writes them this frame)
    ts::Guarded<Skeletons> skeletons{ ts::Named{"skeletons"}, entity_count };
    ts::Guarded<Nav_mesh> nav_mesh{ ts::Named{"nav_mesh"}, entity_count };
    ts::Guarded<Renderables> renderables{ ts::Named{"renderables"}, entity_count };
    ts::Guarded<Asset_source> asset_source{ ts::Named{"asset_source"}, entity_count };

    // single-writer outputs (writer named by the type)
    ts::Guarded<Input> input{ ts::Named{"input"}, entity_count };
    ts::Guarded<Camera> camera{ ts::Named{"camera"}, entity_count };
    ts::Guarded<Net> net{ ts::Named{"net"}, entity_count };
    ts::Guarded<Script_events> script_events{ ts::Named{"script_events"}, entity_count };
    ts::Guarded<Assets> assets{ ts::Named{"assets"}, entity_count };
    // Streaming stages loaded assets here as each load completes; the asset_commit node applies
    // the batch (declared after `assets`, so it destructs before it -- ~Deferred touches the
    // target's pipe).
    ts::Deferred<Assets> assets_stream{ assets };
    // Detached load coroutines in flight; the teardown flush waits on it (see ~World).
    std::atomic<int> streaming_inflight{ 0 };
    ts::Guarded<Nav_tiles> nav_tiles{ ts::Named{"nav_tiles"}, entity_count };
    ts::Guarded<Combat> combat{ ts::Named{"combat"}, entity_count };
    ts::Guarded<Economy> economy{ ts::Named{"economy"}, entity_count };
    ts::Guarded<Quests> quests{ ts::Named{"quests"}, entity_count };
    ts::Guarded<Paths> paths{ ts::Named{"paths"}, entity_count };
    ts::Guarded<Intents> intents{ ts::Named{"intents"}, entity_count };
    ts::Guarded<Anim_pose> anim_pose{ ts::Named{"anim_pose"}, entity_count };
    ts::Guarded<Local_xf> local_xf{ ts::Named{"local_xf"}, entity_count };
    ts::Guarded<Skin_matrices> skin_matrices{ ts::Named{"skin_matrices"}, entity_count };
    ts::Guarded<Velocities> velocities{ ts::Named{"velocities"}, entity_count };
    ts::Guarded<Broad_pairs> broad_pairs{ ts::Named{"broad_pairs"}, entity_count };
    ts::Guarded<Contacts> contacts{ ts::Named{"contacts"}, entity_count };
    ts::Guarded<Bodies> bodies{ ts::Named{"bodies"}, entity_count };
    ts::Guarded<Visibility> visibility{ ts::Named{"visibility"}, entity_count };
    ts::Guarded<Vis_final> vis_final{ ts::Named{"vis_final"}, entity_count };
    ts::Guarded<Shadow_map> shadow_map{ ts::Named{"shadow_map"}, entity_count };

    // the published transforms: staged by propagation, flipped by the publish node
    ts::Versioned<Transforms> transforms{ ts::Named{"transforms"} };

    // the published gameplay snapshot: the optimised variant packs the trio's
    // results here and publishes them for NEXT frame's AI (one-frame-latent
    // perception). Unused in the baseline (empty, no stages -- harmless).
    ts::Versioned<Gameplay> gameplay{ ts::Named{"gameplay"} };

    ts::Guarded<Cloth> cloth{ ts::Named{"cloth"}, entity_count };
    ts::Guarded<Particles> particles{ ts::Named{"particles"}, entity_count };
    ts::Guarded<Audio_out> audio_out{ ts::Named{"audio_out"}, entity_count };
    ts::Guarded<Vfx> vfx{ ts::Named{"vfx"}, entity_count };
    ts::Guarded<UI> UI{ ts::Named{"UI"}, entity_count };
    ts::Guarded<Replication> replication{ ts::Named{"replication"}, entity_count };
    ts::Guarded<Stats> stats{ ts::Named{"stats"}, entity_count };
    ts::Guarded<GC> gc{ ts::Named{"gc"}, entity_count };
    ts::Guarded<Debug_out> debug_out{ ts::Named{"debug_out"}, entity_count };

    // the render queue: baseline producers write it directly, optimised producers
    // stage into `draw_staged` and submit applies the batch
    ts::Guarded<Draw_lists> draw_lists{ ts::Named{"draw_lists"} };
    ts::Deferred<Draw_lists> draw_staged{ draw_lists };   // references an earlier member -- fine

    // Streaming loads run detached and can finish after the frame that launched them, so at
    // teardown some may still be staging into `assets_stream`. Flush on shutdown: wait for the
    // in-flight loaders to drain, then apply the final batch as one write -- leaving no staged
    // commands for `~Deferred` to reject as lost writes. (A user-declared destructor keeps this
    // an aggregate -- only user-declared constructors would forfeit `World world{ entities }`.)
    ~World()
    {
        while (streaming_inflight.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();
        assets_stream.commit().sync();
    }
};

// --- system budgets (ms at scale 1.0) ---------------------------------------------
// Single source of truth: the tick_* bodies burn these and `serial_budget_ms` sums
// them. Grouped by pipeline stage.
namespace budget
{
// frame head + streaming + navmesh rebuild
constexpr double input = 0.1, camera = 0.1, networking = 0.5, scripting = 0.6, streaming = 1.5, navmesh_rebuild = 0.6;
// gameplay trio
constexpr double combat = 0.8, economy = 0.7, quests = 0.7;
// navigation + AI (the snapshot is optimised-only, outside the baseline serial budget)
constexpr double navigation = 2.5, AI = 1.5, gameplay_snapshot = 0.1;
// animation chain
constexpr double anim_graph = 2.5, ik_post = 0.8, skinning = 1.0;
// physics pipeline
constexpr double broadphase = 0.8, narrowphase = 1.5, solver = 2.8, finalize = 0.4;
// transform propagation
constexpr double propagation = 1.0;
// render pipeline
constexpr double frustum_cull = 0.9, occlusion_cull = 0.9, shadow = 1.0, cmd_record = 3.0, submit = 0.8;
// post-flip reader + audio/vfx
constexpr double cloth = 2.0, audio = 1.6, vfx = 0.6;
// draw producers
constexpr double particles = 2.0, UI = 1.5;
// terminal leaves
constexpr double replication = 0.4, stats = 0.3, gc = 1.0, debug_overlay = 0.2;
} // namespace budget

// --- the systems: index -----------------------------------------------------------
// Free functions with typed parameters -- the const-ness IS the access declaration
// the graph reads. Implementations follow `build_frame_graph`; this index is the
// frame's table of contents.

// frame head
void tick_input(Input&);
void tick_camera(const Input&, Camera&);
void tick_networking(const Input&, Net&);
void tick_scripting(const Input&, const Net&, Script_events&);
void tick_streaming(ts::Guarded<Asset_source>&, ts::Deferred<Assets>&, std::atomic<int>&, const Input&);
void tick_navmesh_rebuild(const Input&, Nav_tiles&);
// gameplay trio (combat splits per-entity when `parallel`)
void tick_combat(const Transforms& prev_xf, const Input&, const Net&, const Script_events&, Combat&, bool parallel);
void tick_economy(const Transforms& prev_xf, const Input&, const Net&, const Script_events&, Economy&);
void tick_quests(const Transforms& prev_xf, const Input&, const Net&, const Script_events&, Quests&);
// navigation + AI (baseline reads this frame's trio; optimised reads last frame's snapshot)
void tick_navigation(const Nav_mesh&, const Nav_tiles&, const Transforms& prev_xf, Paths&);
void tick_AI(ts::Guarded<Nav_mesh>&, const Transforms& prev_xf, const Paths&, const Combat&, const Economy&,
    const Quests&, Intents&);
void tick_AI_prev(ts::Guarded<Nav_mesh>&, const Transforms& prev_xf, const Paths&, const Gameplay& prev_gameplay,
    Intents&);
void tick_gameplay_snapshot(const Combat&, const Economy&, const Quests&, ts::Recorder<Gameplay>&);
// animation chain (ik_post splits per-character when `parallel`)
void tick_anim_graph(const Skeletons&, const Intents&, Anim_pose&);
void tick_ik_post(const Anim_pose&, Local_xf&, bool parallel);
void tick_skinning(const Local_xf&, Skin_matrices&);
// physics pipeline
void tick_broadphase(const Bodies&, Broad_pairs&);
void tick_narrowphase(const Broad_pairs&, Contacts&);
void tick_solver(const Contacts&, const Combat&, Velocities&);
void tick_finalize(const Velocities&, Bodies&);
// transform propagation (stages this frame's transforms; the flip publishes)
void tick_propagation(const Local_xf&, const Bodies&, const Velocities&, ts::Recorder<Transforms>&);
// render pipeline (reads LAST frame's transforms)
void tick_frustum_cull(const Transforms& prev_xf, const Camera&, const Renderables&, Visibility&);
void tick_occlusion_cull(const Transforms& prev_xf, const Visibility&, Vis_final&);
void tick_shadow(const Transforms& prev_xf, const Skeletons&, Shadow_map&);
// draw producers (return the batch size for the queue; see the add_* helpers)
int tick_cmd_record(const Vis_final&, const Shadow_map&, const Renderables&);
int tick_particles(const Transforms& prev_xf, Particles&);
int tick_UI(const Quests&, UI&, bool parallel);
// post-flip reader + leaves
void tick_cloth(const Transforms& xf, Cloth&);
void tick_audio(const Transforms& prev_xf, Audio_out&);
void tick_vfx(const Transforms& prev_xf, const Particles&, Vfx&);
void tick_replication(const Combat&, const Economy&, const Quests&, const Intents&, Replication&);
void tick_stats(const Combat&, const Economy&, const Bodies&, const Visibility&, Stats&);
void tick_gc(const Assets&, const Renderables&, const Particles&, GC&);
void tick_debug_overlay(const Economy&, const Transforms&);

// cost/read helpers used by the builder's inline bodies (definitions below)
void spin(double ms);
void read_all(const Float_store&);
void read_all(const Transforms&);
void parallel_cost(double total_ms);

// draw-producer node builders: the baseline/optimised access shapes differ, so each
// picks a shape on `opt` (definitions below build_frame_graph)
ts::Graph_node add_cmd_record(ts::Static_task_graph&, World&, bool opt);
ts::Graph_node add_particles(ts::Static_task_graph&, World&, bool opt);
ts::Graph_node add_UI(ts::Static_task_graph&, World&, bool opt);
ts::Graph_node add_submit(ts::Static_task_graph&, World&, bool opt);

// navigation + AI node builders (extracted so build_frame_graph stays a node list). AI
// picks its inputs on `opt` -- last frame's snapshot (optimised) vs this frame's trio.
ts::Graph_node add_navigation(ts::Static_task_graph&, World&);
ts::Graph_node add_AI(ts::Static_task_graph&, World&, bool opt);

// --- the frame graph --------------------------------------------------------------

// Build the frame. Construction is shared; `opt` branches the few nodes whose access
// differs (draw producers, AI's inputs, cloth's version) and parameterises the two
// body-level splits (combat, ik_post).
ts::Static_task_graph build_frame_graph(World& world, Frame_variant variant, const char* DOT_path = nullptr)
{
    const bool opt = variant == Frame_variant::optimised;
    ts::Static_task_graph graph;

    // Priorities model importance, not measured wins, and are IDENTICAL in both
    // variants -- compilation ignores them (they only order ready tasks in the
    // scheduler's queues): `low` on the deferrable terminal leaves, `high` on the
    // longest pole and the present deadline.

    // Frame head.
    graph.add_node("input", &tick_input, world.input);
    graph.add_node("camera", &tick_camera, world.input, world.camera);
    graph.add_node("networking", &tick_networking, world.input, world.net);
    graph.add_node("scripting", &tick_scripting, world.input, world.net, world.script_events);
    // Streaming declares only `input`: it stages loaded assets into `assets_stream` (grant-free)
    // and reads `asset_source` through its own async loads -- neither is a declared node access.
    graph.add_node("streaming",
        [&world](const Input& in)
        { tick_streaming(world.asset_source, world.assets_stream, world.streaming_inflight, in); },
        world.input);
    graph.add_node("navmesh_rebuild", &tick_navmesh_rebuild, world.input, world.nav_tiles);

    // Gameplay trio: shared inputs, disjoint outputs -> runs in parallel. Combat is
    // parallelised in the optimised variant (a critical bar); economy/quests stay serial.
    graph.add_node("combat",   // L2: per-entity split when opt
        [opt](const Transforms& xf, const Input& in, const Net& net, const Script_events& ev, Combat& c)
        { tick_combat(xf, in, net, ev, c, opt); },
        world.transforms.state(), world.input, world.net, world.script_events, world.combat);
    graph.add_node("economy", &tick_economy, world.transforms.state(), world.input, world.net, world.script_events, world.economy);
    graph.add_node("quests", &tick_quests, world.transforms.state(), world.input, world.net, world.script_events, world.quests);

    // Navigation + AI. Baseline AI reads THIS frame's trio (the trio binds AI on the
    // critical path); optimised reads last frame's gameplay snapshot instead, deleting
    // the trio->AI edges so AI starts as soon as its paths are ready -- the flagship cut.
    add_navigation(graph, world);
    add_AI(graph, world, opt);

    // Gameplay snapshot + flip (optimised only): pack this frame's trio into the
    // versioned gameplay store and publish it for NEXT frame's AI. AI (above) reads
    // the PREVIOUS version, so the flip orders after it; nothing this frame waits on
    // the snapshot.
    if (opt)   // L1: publish the snapshot next frame's AI reads
    {
        auto gp_snapshot = graph.add_node("gameplay_snapshot",
            [rec = world.gameplay.recorder()](const Combat& c, const Economy& e, const Quests& q) mutable
            { tick_gameplay_snapshot(c, e, q, rec); },
            world.combat, world.economy, world.quests);
        graph.add_node("gameplay_flip", ts::publish_body(world.gameplay), world.gameplay.state()).after(gp_snapshot);
    }

    // Animation chain.
    graph.add_node("anim_graph", &tick_anim_graph, world.skeletons, world.intents, world.anim_pose);
    graph.add_node("ik_post", [opt](const Anim_pose& pose, Local_xf& lx) { tick_ik_post(pose, lx, opt); },   // L2
        world.anim_pose, world.local_xf);
    graph.add_node("skinning", &tick_skinning, world.local_xf, world.skin_matrices);

    // Physics pipeline.
    graph.add_node("broadphase", &tick_broadphase, world.bodies, world.broad_pairs);
    graph.add_node("narrowphase", &tick_narrowphase, world.broad_pairs, world.contacts);
    graph.add_node("solver", &tick_solver, world.contacts, world.combat, world.velocities).priority(ts::Priority::high);
    graph.add_node("finalize", &tick_finalize, world.velocities, world.bodies);

    // Propagation: this frame's transforms from animation + physics, staged grant-free
    // (the physics->propagation edge derives from two conflicts: bodies + velocities).
    auto propagation = graph.add_node("propagation",
        [rec = world.transforms.recorder()](const Local_xf& lx, const Bodies& b, const Velocities& v) mutable
        { tick_propagation(lx, b, v, rec); },
        world.local_xf, world.bodies, world.velocities);

    // Render pipeline -- reads LAST frame's transforms (so it overlaps this frame's
    // simulation), which means every node here must run BEFORE the flip. That is intent, so
    // it is declared as explicit edges below rather than left to the direction `compile()`
    // happens to give the transforms conflict: a derived edge exists for safety and either
    // direction is race-free, so declaration order is not a specification (guide §6.5).
    auto frustum_cull = graph.add_node("frustum_cull", &tick_frustum_cull, world.transforms.state(), world.camera, world.renderables, world.visibility);
    auto occlusion_cull = graph.add_node("occlusion_cull", &tick_occlusion_cull, world.transforms.state(), world.visibility, world.vis_final);
    auto shadow = graph.add_node("shadow", &tick_shadow, world.transforms.state(), world.skeletons, world.shadow_map);
    auto cmd_record = add_cmd_record(graph, world, opt);
    auto particles = add_particles(graph, world, opt);
    auto UI_node = add_UI(graph, world, opt);
    auto submit = add_submit(graph, world, opt);
    submit.priority(ts::Priority::high);
    // Submit consumes what the producers emit -- intent, in both variants. The baseline
    // ALSO has a `draw_lists` conflict pointing the same way, but relying on that would be
    // relying on the order the nodes happen to be added; the optimised variant stages
    // grant-free, so there is no conflict at all and the edge is the only ordering.
    submit.after(cmd_record, particles, UI_node);

    // Off-path leaves.
    auto audio = graph.add_node("audio", &tick_audio, world.transforms.state(), world.audio_out).priority(ts::Priority::low);
    auto vfx = graph.add_node("vfx", &tick_vfx, world.transforms.state(), world.particles, world.vfx);
    graph.add_node("replication", &tick_replication, world.combat, world.economy, world.quests, world.intents, world.replication);
    graph.add_node("stats", &tick_stats, world.combat, world.economy, world.bodies, world.visibility, world.stats);
    // The streaming commit slot: applies the assets staged so far as one write. It writes
    // `assets`, so the conflict edge to gc (which reads `assets`) orders it before gc; it has no
    // edge to streaming (staging is grant-free), so it commits whatever has arrived -- last
    // frame's loads are fine.
    graph.add_node("asset_commit", [&world](Assets& a) { (void)a; world.assets_stream.commit(); },
        world.assets).priority(ts::Priority::low);
    graph.add_node("gc", &tick_gc, world.assets, world.renderables, world.particles, world.gc);
    auto debug_overlay = graph.add_node("debug_overlay",   // bare generic lambda: modes probed
        [](const auto& economy, const auto& xf) { tick_debug_overlay(economy, xf); },
        world.economy, world.transforms.state()).priority(ts::Priority::low);

    // The flip publishes this frame's transforms. Cloth's version is a lever: the
    // baseline reads the FRESH version (declared after the flip -> the frame's tail
    // node); the optimised variant reads LAST frame's (declared before, like audio) --
    // one frame of cloth latency is invisible, and cloth fills the idle capacity the
    // shortened spine leaves. Structural (the version is set by declaration order vs
    // the flip).
    ts::Graph_node cloth;
    if (opt)   // L4: cloth on last frame's transforms
        cloth = graph.add_node("cloth", &tick_cloth, world.transforms.state(), world.cloth);
    auto flip = graph.add_node("flip", ts::publish_body(world.transforms), world.transforms.state());
    flip.after(propagation);
    // Which VERSION each transforms reader sees is the point of the whole render pipeline,
    // so it is declared, not inferred from where the nodes happen to sit in this function.
    // Every reader above must precede the flip to see last frame's transforms; `cloth` in
    // the baseline must follow it to see this frame's. Each of these coincides with a
    // derived edge in the same direction -- `compile()` dedups them -- but the derived edge
    // is there for safety, and its direction is not a specification (guide §6.5).
    flip.after(frustum_cull, occlusion_cull, shadow, cmd_record, particles, UI_node, submit);
    flip.after(audio, vfx, debug_overlay);
    if (opt)
        flip.after(cloth);
    else
        graph.add_node("cloth", &tick_cloth, world.transforms.state(), world.cloth).after(flip);

    graph.compile(DOT_path);
    return graph;
}

// --- the draw-producer node builders ----------------------------------------------
// The two shapes differ in access, so each branches on `opt`. Baseline writes
// `draw_lists` directly (a write grant -> the producers serialise on the queue);
// optimised stages into `draw_staged` grant-free (L3: they go wide, submit applies
// the batch). The system logic is in the tick_* functions; these only pick the shape.

// Command recording: the heavy render producer (batches draw calls from the visible
// set + shadows).
// Navigation: paths for this frame against the nav service + baked tiles, reading last
// frame's transforms. One node, identical in both variants -- extracted so the builder
// reads as a flat node list.
ts::Graph_node add_navigation(ts::Static_task_graph& graph, World& world)
{
    return graph.add_node("navigation", &tick_navigation, world.nav_mesh, world.nav_tiles, world.transforms.state(), world.paths);
}

// AI: nav-query core + intents, reading either this frame's gameplay trio (baseline) or last
// frame's published gameplay snapshot (optimised). The snapshot read deletes the trio->AI
// edges -- the flagship critical-path cut -- so AI runs as soon as its paths are ready. Both
// shapes are thin wrappers over tick_AI / tick_AI_prev.
ts::Graph_node add_AI(ts::Static_task_graph& graph, World& world, bool opt)
{
    if (opt)
    {
        return graph.add_node("AI",   // L1: snapshot AI
            [&world](const Transforms& xf, const Paths& p, const Gameplay& gp, Intents& i)
            { tick_AI_prev(world.nav_mesh, xf, p, gp, i); },
            world.transforms.state(), world.paths, world.gameplay.state(), world.intents);
    }
    return graph.add_node("AI",
        [&world](const Transforms& xf, const Paths& p, const Combat& c, const Economy& e, const Quests& q, Intents& i)
        { tick_AI(world.nav_mesh, xf, p, c, e, q, i); },
        world.transforms.state(), world.paths, world.combat, world.economy, world.quests, world.intents);
}

ts::Graph_node add_cmd_record(ts::Static_task_graph& graph, World& world, bool opt)
{
    if (!opt)
    {
        return graph.add_node("cmd_record",
            [](const Vis_final& vis, const Shadow_map& shadows, const Renderables& r, Draw_lists& draws)
            { draws.push_batch(tick_cmd_record(vis, shadows, r)); },
            world.vis_final, world.shadow_map, world.renderables, world.draw_lists);
    }
    return graph.add_node("cmd_record",   // L3: stage grant-free
        [rec = world.draw_staged.recorder()](const Vis_final& vis, const Shadow_map& shadows, const Renderables& r) mutable
        {
            int n = tick_cmd_record(vis, shadows, r);
            rec.stage([n](Draw_lists& d) { d.push_batch(n); });
        },
        world.vis_final, world.shadow_map, world.renderables);
}

ts::Graph_node add_particles(ts::Static_task_graph& graph, World& world, bool opt)
{
    if (!opt)
    {
        return graph.add_node("particles",
            [](const Transforms& prev_xf, Particles& p, Draw_lists& draws)
            { draws.push_batch(tick_particles(prev_xf, p)); },
            world.transforms.state(), world.particles, world.draw_lists);
    }
    return graph.add_node("particles",   // L3: stage grant-free
        [rec = world.draw_staged.recorder()](const Transforms& prev_xf, Particles& p) mutable
        {
            int n = tick_particles(prev_xf, p);
            rec.stage([n](Draw_lists& d) { d.push_batch(n); });
        },
        world.transforms.state(), world.particles);
}

// UI is a double lever: the baseline lays widgets out serially AND writes the queue
// directly; the optimised variant lays out in parallel AND stages grant-free.
ts::Graph_node add_UI(ts::Static_task_graph& graph, World& world, bool opt)
{
    if (!opt)
    {
        return graph.add_node("UI",
            [](const Quests& quests, UI& u, Draw_lists& draws) { draws.push_batch(tick_UI(quests, u, false)); },
            world.quests, world.UI, world.draw_lists);
    }
    return graph.add_node("UI",   // L3: stage grant-free; L2: parallel layout
        [rec = world.draw_staged.recorder()](const Quests& quests, UI& u) mutable
        {
            int n = tick_UI(quests, u, true);
            rec.stage([n](Draw_lists& d) { d.push_batch(n); });
        },
        world.quests, world.UI);
}

// Submit: consumes the draw queue. Baseline just reads the directly-written counts;
// optimised commits the staged batch under its own grant.
ts::Graph_node add_submit(ts::Static_task_graph& graph, World& world, bool opt)
{
    if (!opt)
    {
        return graph.add_node("submit",
            [](const Transforms& prev_xf, Draw_lists& dl)
            {
                read_all(prev_xf);
                drawn.fetch_add(dl.count(), std::memory_order_relaxed);
                parallel_cost(budget::submit);
                dl.clear();
            },
            world.transforms.state(), world.draw_lists);
    }
    return graph.add_node("submit",
        [&world](const Transforms& prev_xf, Draw_lists& dl)
        {
            read_all(prev_xf);
            world.draw_staged.commit();
            drawn.fetch_add(dl.count(), std::memory_order_relaxed);
            parallel_cost(budget::submit);
            dl.clear();
        },
        world.transforms.state(), world.draw_lists);
}

// --- cost mocking + shared helpers ------------------------------------------------

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
    ts::parallel_for(cost_slices, [total_ms](int) { spin(total_ms / cost_slices); });
}

// --- the systems: implementations -------------------------------------------------

// Frame head ----

void tick_input(Input& input)
{
    fill(input, 1.0f);
    spin(budget::input);
}

void tick_camera(const Input& input, Camera& camera)
{
    read_all(input);
    fill(camera, 1.0f);
    spin(budget::camera);
}

void tick_networking(const Input& input, Net& net)
{
    read_all(input);
    fill(net, 1.0f);
    spin(budget::networking);
}

// Gameplay scripting / VM tick: dispatches this frame's gameplay events. A real
// engine burns real time here; the trio reads its output.
void tick_scripting(const Input& input, const Net& net, Script_events& events)
{
    read_all(input);
    read_all(net);
    fill(events, 1.0f);
    spin(budget::scripting);
}

// Streaming: async loads from the read-only source, overlapping the node's own
// decompression cost. A real streaming system writes each loaded asset into the table WHEN
// its load completes, not at frame start -- so the loader stages each result into
// `assets_stream` (a `Deferred<Assets>`, grant-free) as it arrives, and the asset_commit node
// applies the batch at its slot. Next frame is fine: streaming latency is not frame-critical.
// This is the recommended shape (stage outward writes, apply at a commit slot) -- the loader
// holds no grant across its multi-frame wait, so there is nothing to climb a rank away from.
void tick_streaming(ts::Guarded<Asset_source>& asset_source, ts::Deferred<Assets>& assets_stream,
    std::atomic<int>& inflight, const Input& input)
{
    read_all(input);

    // The loads run detached, fire-and-forget. Kicked off through a detached `ts::launch`, which
    // inherits none of this node's grants (docs/coroutine-first.md §2): the loader created inside
    // it snapshots an empty context, so it holds nothing while it awaits `asset_source`. Four
    // loads fire eagerly and run concurrently; the coroutine stages each result as it arrives and
    // counts the batch after the last. `inflight` gates the teardown flush (see ~World): bumped
    // here, dropped at the loader's end after the last stage.
    inflight.fetch_add(1, std::memory_order_relaxed);
    ts::launch([&asset_source, &assets_stream, &inflight]
    {
        [](ts::Guarded<Asset_source>& src, ts::Deferred<Assets>& stage, std::atomic<int>& live) -> ts::Task<void>
        {
            auto rec = stage.recorder();
            auto load = [](const Asset_source& s) { spin(0.2); return s.size() > 0 ? s.get(0) : 1.0f; };
            ts::Task<float> a = src.async(load);
            ts::Task<float> b = src.async(load);
            ts::Task<float> c = src.async(load);
            ts::Task<float> d = src.async(load);
            float va = co_await a; rec.stage([va](Assets& t) { if (t.size() > 0) t.set(0, va); }); streamed.fetch_add(1, std::memory_order_relaxed);
            float vb = co_await b; rec.stage([vb](Assets& t) { if (t.size() > 1) t.set(1, vb); }); streamed.fetch_add(1, std::memory_order_relaxed);
            float vc = co_await c; rec.stage([vc](Assets& t) { if (t.size() > 2) t.set(2, vc); }); streamed.fetch_add(1, std::memory_order_relaxed);
            float vd = co_await d; rec.stage([vd](Assets& t) { if (t.size() > 3) t.set(3, vd); }); streamed.fetch_add(1, std::memory_order_relaxed);
            batches.fetch_add(1, std::memory_order_relaxed);
            live.fetch_sub(1, std::memory_order_release);   // last: all four staged
        }(asset_source, assets_stream, inflight);
    });

    spin(budget::streaming);
}

// A background nav-mesh tile rebuild: streams and bakes navigation tiles into its
// own store, which navigation reads. An independent writer (fed by input) --
// off the critical path, but its output feeds navigation.
void tick_navmesh_rebuild(const Input& input, Nav_tiles& tiles)
{
    read_all(input);
    fill(tiles, 1.0f);
    spin(budget::navmesh_rebuild);
}

// Gameplay trio: shares inputs (last frame's transforms, input, net, script
// events), owns disjoint outputs -> runs in parallel.
// Combat runs a threat/damage pass per entity. The baseline runs it serially; the
// optimised variant parallelises it across entities -- the trace shows combat as
// one of the two fattest bars on the critical path, so a per-entity split is the
// obvious cut. `parallel` picks the shape.
void tick_combat(const Transforms& prev_xf, const Input& input, const Net& net, const Script_events& events,
    Combat& combat, bool parallel)
{
    read_all(prev_xf);
    read_all(input);
    read_all(net);
    read_all(events);
    if (parallel)   // L2
    {
        parallel_fill(combat, 1.0f, budget::combat);
    }
    else
    {
        fill(combat, 1.0f);
        spin(budget::combat);
    }
}

void tick_economy(const Transforms& prev_xf, const Input& input, const Net& net, const Script_events& events,
    Economy& economy)
{
    read_all(prev_xf);
    read_all(input);
    read_all(net);
    read_all(events);
    fill(economy, 1.0f);
    spin(budget::economy);
}

void tick_quests(const Transforms& prev_xf, const Input& input, const Net& net, const Script_events& events,
    Quests& quests)
{
    read_all(prev_xf);
    read_all(input);
    read_all(net);
    read_all(events);
    fill(quests, 1.0f);
    spin(budget::quests);
}

void tick_navigation(const Nav_mesh& nav_mesh, const Nav_tiles& tiles, const Transforms& prev_xf, Paths& paths)
{
    read_all(nav_mesh);
    read_all(tiles);
    read_all(prev_xf);
    parallel_fill(paths, 1.0f, budget::navigation);
}

// AI's nav-query core, shared by both variants: per-agent path queries against
// the read-only nav service via async -- concurrent readers on other workers,
// overlapping AI's own logic. Speculative: each query runs a longer budget than
// AI needs and AI cancels the batch once done (cooperative early-out via the
// trailing token). Writes this frame's intents.
void ai_nav_and_intents(ts::Guarded<Nav_mesh>& nav_service, Intents& intents)
{
    ts::Cancellation_source nav_cancel;
    constexpr int queries = 6;
    for (int q = 0; q < queries; ++q)
        nav_service.async([](const Nav_mesh& n, ts::Cancellation_token tok)
        {
            update_max(nav_peak, nav_active.fetch_add(1) + 1);
            bool bailed = false;
            auto start = std::chrono::steady_clock::now();
            auto query_budget = std::chrono::duration<double, std::milli>(2.0 * time_scale);
            while (std::chrono::steady_clock::now() - start < query_budget)
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

    parallel_cost(budget::AI);
    nav_cancel.request_cancel();
    fill(intents, 1.0f);
}

// Baseline AI: reads THIS frame's trio outputs -- so the trio binds AI on the
// critical path (the trace shows exactly this). The optimised variant reads last
// frame's gameplay snapshot instead (tick_AI_prev), deleting the trio->AI edges.
void tick_AI(ts::Guarded<Nav_mesh>& nav_service, const Transforms& prev_xf, const Paths& paths, const Combat& combat,
    const Economy& economy, const Quests& quests, Intents& intents)
{
    read_all(prev_xf);
    read_all(paths);
    read_all(combat);
    read_all(economy);
    read_all(quests);
    ai_nav_and_intents(nav_service, intents);
}

// Optimised AI: reads last frame's gameplay snapshot (a `Versioned` published at
// the end of the previous frame) in place of this frame's trio. One frame of AI
// latency is standard, and it lets AI start as soon as its paths are ready
// instead of waiting for the gameplay trio -- the flagship critical-path cut.
void tick_AI_prev(ts::Guarded<Nav_mesh>& nav_service, const Transforms& prev_xf, const Paths& paths,
    const Gameplay& prev_gameplay, Intents& intents)
{
    read_all(prev_xf);
    read_all(paths);
    read_all(prev_gameplay);
    ai_nav_and_intents(nav_service, intents);
}

// Gameplay snapshot: packs this frame's trio results into the versioned gameplay
// store for next frame's AI. Off the critical path (nothing this frame waits on
// it -- it feeds the NEXT frame via the published version). Optimised only.
void tick_gameplay_snapshot(const Combat& combat, const Economy& economy, const Quests& quests,
    ts::Recorder<Gameplay>& rec)
{
    read_all(combat);
    read_all(economy);
    read_all(quests);
    std::vector<float> batch(static_cast<std::size_t>(combat.size()), 1.0f);
    spin(budget::gameplay_snapshot);   // packing the snapshot
    rec.stage([batch = std::move(batch)](Gameplay& gameplay) { gameplay.apply(batch); });
}

// Animation chain: graph eval -> IK/post -> skinning.
void tick_anim_graph(const Skeletons& skeletons, const Intents& intents, Anim_pose& anim_pose)
{
    read_all(skeletons);
    read_all(intents);
    parallel_fill(anim_pose, 1.0f, budget::anim_graph);
}

// IK / post: refines the pose into the final local transforms. IK is per
// character -- embarrassingly parallel across characters -- so the baseline's
// serial pass is a critical-path bar the optimised variant splits. Writes
// local_xf = 2.0 (half of the deterministic transform output).
void tick_ik_post(const Anim_pose& anim_pose, Local_xf& local_xf, bool parallel)
{
    read_all(anim_pose);
    if (parallel)   // L2
    {
        parallel_fill(local_xf, 2.0f, budget::ik_post);
    }
    else
    {
        fill(local_xf, 2.0f);
        spin(budget::ik_post);
    }
}

// Skinning: matrix palette from the final pose, consumed by next frame's render.
void tick_skinning(const Local_xf& local_xf, Skin_matrices& skin)
{
    read_all(local_xf);
    parallel_fill(skin, 1.0f, budget::skinning);
}

// Physics pipeline: broadphase -> narrowphase -> solver -> finalize. Broadphase
// reads body positions (last frame's, before finalize rewrites them this frame);
// the solver integrates velocities; finalize integrates positions. Finalize
// writes bodies = 3.0 (the other half of the deterministic transform output).
void tick_broadphase(const Bodies& bodies, Broad_pairs& pairs)
{
    read_all(bodies);
    parallel_fill(pairs, 1.0f, budget::broadphase);
}

void tick_narrowphase(const Broad_pairs& pairs, Contacts& contacts)
{
    read_all(pairs);
    parallel_fill(contacts, 1.0f, budget::narrowphase);
}

void tick_solver(const Contacts& contacts, const Combat& combat, Velocities& velocities)
{
    read_all(contacts);
    read_all(combat);
    parallel_fill(velocities, 1.0f, budget::solver);
}

void tick_finalize(const Velocities& velocities, Bodies& bodies)
{
    read_all(velocities);
    parallel_fill(bodies, 3.0f, budget::finalize);
}

// Propagation: velocity-aware interpolation of this frame's transforms from
// animation (local_xf) + physics (bodies), staged into the versioned store; the
// flip node publishes the batch.
void tick_propagation(const Local_xf& local_xf, const Bodies& bodies, const Velocities& velocities,
    ts::Recorder<Transforms>& rec)
{
    read_all(velocities);
    std::vector<float> out(static_cast<std::size_t>(local_xf.size()));
    for (int i = 0, n = local_xf.size(); i < n; ++i)
        out[static_cast<std::size_t>(i)] = local_xf.get(i) + bodies.get(i);
    parallel_cost(budget::propagation);
    rec.stage([batch = std::move(out)](Transforms& t) { t.apply(batch); });
}

// Render pipeline: consumes LAST frame's transforms (declared before the flip),
// so it overlaps this frame's simulation -- the render thread with one frame of
// latency. Its own working stores (visibility, shadows) are this-frame.
void tick_frustum_cull(const Transforms& prev_xf, const Camera& camera, const Renderables& renderables,
    Visibility& visibility)
{
    read_all(prev_xf);
    read_all(camera);
    read_all(renderables);
    parallel_fill(visibility, 1.0f, budget::frustum_cull);
}

void tick_occlusion_cull(const Transforms& prev_xf, const Visibility& visibility, Vis_final& vis_final)
{
    read_all(prev_xf);
    read_all(visibility);
    parallel_fill(vis_final, 1.0f, budget::occlusion_cull);
}

void tick_shadow(const Transforms& prev_xf, const Skeletons& skeletons, Shadow_map& shadow_map)
{
    read_all(prev_xf);
    read_all(skeletons);
    parallel_fill(shadow_map, 1.0f, budget::shadow);
}

// Command recording: batches draw calls from the visible set + shadows; returns
// the batch size for the queue (pushed or staged by the add_cmd_record shapes).
int tick_cmd_record(const Vis_final& vis, const Shadow_map& shadows, const Renderables& renderables)
{
    read_all(vis);
    read_all(shadows);
    read_all(renderables);
    parallel_cost(budget::cmd_record);
    return vis.size();
}

int tick_particles(const Transforms& prev_xf, Particles& particles)
{
    read_all(prev_xf);
    parallel_fill(particles, 1.0f, budget::particles);
    return particles.size() / 4;
}

// UI layout: serial in the baseline (a critical bar), per-widget parallel in the
// optimised variant.
int tick_UI(const Quests& quests, UI& u, bool parallel)
{
    read_all(quests);
    if (parallel)   // L2
    {
        parallel_fill(u, 1.0f, budget::UI);
    }
    else
    {
        fill(u, 1.0f);
        spin(budget::UI);
    }
    return u.size() / 10;
}

// Post-flip reader of the FRESH version. Cloth simulates on this frame's
// transforms (the baseline; the optimised variant declares it pre-flip).
void tick_cloth(const Transforms& xf, Cloth& cloth)
{
    read_all(xf);
    parallel_fill(cloth, 1.0f, budget::cloth);
}

// Audio mixes off LAST frame's transforms (declared before the flip): one frame
// of positional latency is inaudible, and the serial mixer runs from t=0.
void tick_audio(const Transforms& prev_xf, Audio_out& audio_out)
{
    read_all(prev_xf);
    fill(audio_out, 1.0f);
    spin(budget::audio);   // deliberately serial: a single-threaded mixer is realistic
}

void tick_vfx(const Transforms& prev_xf, const Particles& particles, Vfx& vfx)
{
    read_all(prev_xf);
    read_all(particles);
    fill(vfx, 1.0f);
    spin(budget::vfx);
}

// Replication snapshot: reads the gameplay + AI outputs, packs a network
// snapshot. Terminal, multi-read.
void tick_replication(const Combat& combat, const Economy& economy, const Quests& quests, const Intents& intents,
    Replication& rep)
{
    read_all(combat);
    read_all(economy);
    read_all(quests);
    read_all(intents);
    fill(rep, 1.0f);
    spin(budget::replication);
}

// End-of-frame stats: reads across many systems for the telemetry overlay.
// Terminal.
void tick_stats(const Combat& combat, const Economy& economy, const Bodies& bodies, const Visibility& visibility,
    Stats& stats)
{
    read_all(combat);
    read_all(economy);
    read_all(bodies);
    read_all(visibility);
    fill(stats, 1.0f);
    spin(budget::stats);
}

// Memory / GC tick: an incremental collector pass. It scans the frame's allocation-heavy
// resource stores (streamed assets, renderables, spawned particles) to account live memory,
// then does its own bookkeeping. Reading them makes it a terminal consumer like
// stats/replication -- off the critical path, but wired into the frame it collects rather
// than a floating island writing a store no one reads.
void tick_gc(const Assets& assets, const Renderables& renderables, const Particles& particles, GC& gc)
{
    read_all(assets);
    read_all(renderables);
    read_all(particles);
    parallel_fill(gc, 1.0f, budget::gc);
}

// Debug overlay: reads a gameplay store and the transforms for an on-screen HUD. The node
// declares it as a bare generic lambda (`const auto&`), so the graph probes the access mode
// from rvalue-bindability rather than a typed signature -- the one such node in the frame.
void tick_debug_overlay(const Economy& economy, const Transforms& xf)
{
    read_all(economy);
    read_all(xf);
    spin(budget::debug_overlay);
}

// The whole frame run serially -- the baseline node set (the optimised-only
// snapshot/flip are excluded). Both variants' spin budgets are identical.
double serial_budget_ms()
{
    return budget::input + budget::camera + budget::networking + budget::scripting + budget::streaming
         + budget::navmesh_rebuild + budget::combat + budget::economy + budget::quests
         + budget::navigation + budget::AI
         + budget::anim_graph + budget::ik_post + budget::skinning
         + budget::broadphase + budget::narrowphase + budget::solver + budget::finalize
         + budget::propagation
         + budget::frustum_cull + budget::occlusion_cull + budget::shadow + budget::cmd_record + budget::submit
         + budget::cloth + budget::audio + budget::vfx
         + budget::particles + budget::UI
         + budget::replication + budget::stats + budget::gc + budget::debug_overlay;
}

// --- the same frame, composed without a graph --------------------------------------
//
// The comparison case for "what does the static graph actually buy me?". Same `World`,
// same tick_* bodies, no `Static_task_graph`: every system is an ordinary multi-object
// `ts::async` and the schedule is written by hand with `co_await`.
//
// Safety is unchanged, and it was never the graph's: each system takes a mode-aware turn
// on every object it declares, so two conflicting systems never overlap and the access
// harness still fatals on an undeclared touch.
//
// The ordering is what has to be written by hand. Pipe FIFO is not a substitute for the
// graph's conflict edges: a multi-object access enters its links one at a time in canonical
// (pipe-address) order, so a system blocked on its first object has not yet taken its
// slot on the later ones and a system enqueued after it walks straight past. Enqueueing
// the baseline node list in declaration order and awaiting the handles at the end is
// measurably wrong: `frustum_cull` runs before `camera` (its `camera` read lands while
// `camera` is still queued behind `input`), and `submit` takes the draw queue before
// `cmd_record` reaches it, so a frame of draw commands is dropped -- with the transform
// invariant still reading 5 and the harness silent, because every declaration is correct
// and only the order is wrong.
//
// So every edge `compile()` derives is an explicit `co_await` here, and the frame is cut
// into chains so a consumer waits for its own producer rather than for a whole chain.
// Nothing below is checked: a missing `co_await` is a silent race between two systems
// that both declared their access correctly.

// Physics front: broadphase -> narrowphase. Independent of the frame head.
ts::Task<void> gf_physics_front(World& world)
{
    co_await ts::async(&tick_broadphase, world.bodies, world.broad_pairs);
    co_await ts::async(&tick_narrowphase, world.broad_pairs, world.contacts);
}

// Physics back: solver -> finalize. The solver reads this frame's combat, so the chain
// splits here -- the front half has no gameplay input and starts at t=0.
ts::Task<void> gf_physics_back(World& world, ts::Task<void> front, ts::Task<void> combat)
{
    co_await front;
    co_await combat;
    co_await ts::async(&tick_solver, world.contacts, world.combat, world.velocities);
    co_await ts::async(&tick_finalize, world.velocities, world.bodies);
}

ts::Task<void> gf_navigation(World& world, ts::Task<void> navmesh_rebuild)
{
    co_await navmesh_rebuild;
    co_await ts::async(&tick_navigation, world.nav_mesh, world.nav_tiles, world.transforms.state(), world.paths);
}

// AI reads this frame's trio (the baseline shape), so it carries four inbound edges.
ts::Task<void> gf_AI(World& world, ts::Task<void> navigation, ts::Task<void> combat, ts::Task<void> economy,
    ts::Task<void> quests)
{
    co_await navigation;
    co_await combat;
    co_await economy;
    co_await quests;
    co_await ts::async(
        [&world](const Transforms& xf, const Paths& paths, const Combat& c, const Economy& e, const Quests& q,
            Intents& intents) { tick_AI(world.nav_mesh, xf, paths, c, e, q, intents); },
        world.transforms.state(), world.paths, world.combat, world.economy, world.quests, world.intents);
}

// Animation core: anim_graph -> ik_post. Stops at ik_post because propagation waits for
// exactly that, not for skinning -- a chain handle is coarser than an edge, so every
// fan-out point in the frame becomes another chain.
ts::Task<void> gf_anim_core(World& world, ts::Task<void> AI)
{
    co_await AI;
    co_await ts::async(&tick_anim_graph, world.skeletons, world.intents, world.anim_pose);
    co_await ts::async([](const Anim_pose& pose, Local_xf& local_xf) { tick_ik_post(pose, local_xf, false); },
        world.anim_pose, world.local_xf);
}

ts::Task<void> gf_skinning(World& world, ts::Task<void> anim_core)
{
    co_await anim_core;
    co_await ts::async(&tick_skinning, world.local_xf, world.skin_matrices);
}

// Propagation stages this frame's transforms; the flip publishes them. The recorder is
// minted per frame here -- the graph mints one at build time and reuses it, which is one
// of the composition costs the graph-free frame re-pays every frame.
ts::Task<void> gf_propagation(World& world, ts::Task<void> anim_core, ts::Task<void> physics_back)
{
    co_await anim_core;
    co_await physics_back;
    co_await ts::async(
        [rec = world.transforms.recorder()](const Local_xf& local_xf, const Bodies& bodies,
            const Velocities& velocities) mutable { tick_propagation(local_xf, bodies, velocities, rec); },
        world.local_xf, world.bodies, world.velocities);
}

ts::Task<void> gf_frustum_cull(World& world, ts::Task<void> camera)
{
    co_await camera;
    co_await ts::async(&tick_frustum_cull, world.transforms.state(), world.camera, world.renderables,
        world.visibility);
}

ts::Task<void> gf_occlusion_cull(World& world, ts::Task<void> frustum_cull)
{
    co_await frustum_cull;
    co_await ts::async(&tick_occlusion_cull, world.transforms.state(), world.visibility, world.vis_final);
}

ts::Task<void> gf_cmd_record(World& world, ts::Task<void> occlusion_cull, ts::Task<void> shadow)
{
    co_await occlusion_cull;
    co_await shadow;
    co_await ts::async(
        [](const Vis_final& vis, const Shadow_map& shadows, const Renderables& renderables, Draw_lists& draws)
        { draws.push_batch(tick_cmd_record(vis, shadows, renderables)); },
        world.vis_final, world.shadow_map, world.renderables, world.draw_lists);
}

ts::Task<void> gf_UI(World& world, ts::Task<void> quests)
{
    co_await quests;
    co_await ts::async([](const Quests& q, UI& widgets, Draw_lists& draws)
        { draws.push_batch(tick_UI(q, widgets, false)); },
        world.quests, world.UI, world.draw_lists);
}

// Submit consumes the queue, so it waits for all three producers. In the graph these are
// conflict edges on `draw_lists`; here they are three awaits, and forgetting one submits
// a partial frame with nothing to report it.
ts::Task<void> gf_submit(World& world, ts::Task<void> cmd_record, ts::Task<void> particles, ts::Task<void> UI_widgets)
{
    co_await cmd_record;
    co_await particles;
    co_await UI_widgets;
    co_await ts::async([](const Transforms& prev_xf, Draw_lists& draws)
        {
            read_all(prev_xf);
            drawn.fetch_add(draws.count(), std::memory_order_relaxed);
            parallel_cost(budget::submit);
            draws.clear();
        },
        world.transforms.state(), world.draw_lists);
}

ts::Task<void> gf_vfx(World& world, ts::Task<void> particles)
{
    co_await particles;
    co_await ts::async(&tick_vfx, world.transforms.state(), world.particles, world.vfx);
}

ts::Task<void> gf_gc(World& world, ts::Task<void> streaming, ts::Task<void> particles)
{
    co_await streaming;
    co_await particles;
    // The commit slot: apply the assets staged so far (one write on `assets`), then account
    // live memory. Sequencing the commit before the read is the hand-written analogue of the
    // graph's asset_commit->gc conflict edge.
    co_await ts::async([&world](Assets& a) { (void)a; world.assets_stream.commit(); }, world.assets);
    co_await ts::async(&tick_gc, world.assets, world.renderables, world.particles, world.gc);
}

ts::Task<void> gf_replication(World& world, ts::Task<void> AI)
{
    co_await AI;
    co_await ts::async(&tick_replication, world.combat, world.economy, world.quests, world.intents,
        world.replication);
}

ts::Task<void> gf_stats(World& world, ts::Task<void> economy, ts::Task<void> physics_back,
    ts::Task<void> frustum_cull)
{
    co_await economy;
    co_await physics_back;   // covers combat (the solver waits for it) and finalize's bodies
    co_await frustum_cull;
    co_await ts::async(&tick_stats, world.combat, world.economy, world.bodies, world.visibility, world.stats);
}

ts::Task<void> gf_debug_overlay(World& world, ts::Task<void> economy)
{
    co_await economy;
    co_await ts::async([](const auto& economy_store, const auto& xf) { tick_debug_overlay(economy_store, xf); },
        world.economy, world.transforms.state());
}

// The baseline frame with no graph. Reproduces the baseline schedule: same systems, same
// access sets, same edges -- written out.
ts::Task<void> run_frame_graph_free(World& world)
{
    // Branches with no dependency on the frame head start at t=0, as the graph would
    // dispatch them: physics reads last frame's bodies, and the render pipeline reads the
    // last published transforms.
    ts::Task<void> physics_front = gf_physics_front(world);
    ts::Task<void> shadow = ts::async(&tick_shadow, world.transforms.state(), world.skeletons, world.shadow_map);
    ts::Task<void> audio = ts::async(&tick_audio, world.transforms.state(), world.audio_out);
    ts::Task<void> particles = ts::async([](const Transforms& prev_xf, Particles& p, Draw_lists& draws)
        { draws.push_batch(tick_particles(prev_xf, p)); },
        world.transforms.state(), world.particles, world.draw_lists);
    ts::Task<void> vfx = gf_vfx(world, particles);

    // Frame head. Eight systems read `input`, so this one await stands for eight edges.
    co_await ts::async(&tick_input, world.input);

    ts::Task<void> camera = ts::async(&tick_camera, world.input, world.camera);
    ts::Task<void> networking = ts::async(&tick_networking, world.input, world.net);
    ts::Task<void> streaming = ts::async(
        [&world](const Input& in)
        { tick_streaming(world.asset_source, world.assets_stream, world.streaming_inflight, in); },
        world.input);
    ts::Task<void> navmesh_rebuild = ts::async(&tick_navmesh_rebuild, world.input, world.nav_tiles);

    ts::Task<void> frustum_cull = gf_frustum_cull(world, camera);
    ts::Task<void> occlusion_cull = gf_occlusion_cull(world, frustum_cull);
    ts::Task<void> cmd_record = gf_cmd_record(world, occlusion_cull, shadow);
    ts::Task<void> gc = gf_gc(world, streaming, particles);
    ts::Task<void> navigation = gf_navigation(world, navmesh_rebuild);

    co_await networking;
    co_await ts::async(&tick_scripting, world.input, world.net, world.script_events);

    ts::Task<void> combat = ts::async(
        [](const Transforms& xf, const Input& in, const Net& net, const Script_events& ev, Combat& c)
        { tick_combat(xf, in, net, ev, c, false); },
        world.transforms.state(), world.input, world.net, world.script_events, world.combat);
    ts::Task<void> economy = ts::async(&tick_economy, world.transforms.state(), world.input, world.net,
        world.script_events, world.economy);
    ts::Task<void> quests = ts::async(&tick_quests, world.transforms.state(), world.input, world.net,
        world.script_events, world.quests);

    ts::Task<void> UI_widgets = gf_UI(world, quests);
    ts::Task<void> debug_overlay = gf_debug_overlay(world, economy);
    ts::Task<void> physics_back = gf_physics_back(world, physics_front, combat);
    ts::Task<void> AI = gf_AI(world, navigation, combat, economy, quests);
    ts::Task<void> anim_core = gf_anim_core(world, AI);
    ts::Task<void> skinning = gf_skinning(world, anim_core);
    ts::Task<void> replication = gf_replication(world, AI);
    ts::Task<void> propagation = gf_propagation(world, anim_core, physics_back);
    ts::Task<void> stats = gf_stats(world, economy, physics_back, frustum_cull);
    ts::Task<void> submit = gf_submit(world, cmd_record, particles, UI_widgets);

    // The flip. `compile()` derives its predecessors from the explicit `after(propagation)`
    // plus every pre-flip reader of `transforms.state()` -- thirteen of them. Seven awaits
    // cover the set here; the other six are transitive, which is a fact about this frame's
    // shape that a reader has to re-derive by hand every time the frame changes.
    co_await propagation;
    co_await occlusion_cull;   // covers frustum_cull, camera
    co_await shadow;
    co_await submit;           // covers cmd_record, particles, UI
    co_await audio;
    co_await vfx;
    co_await debug_overlay;    // covers economy; propagation covers combat/quests/navigation/AI
    co_await ts::async(ts::publish_body(world.transforms), world.transforms.state());

    // Post-flip: cloth reads the fresh version.
    co_await ts::async(&tick_cloth, world.transforms.state(), world.cloth);

    // Manual completion. `execute()` settles when the last node does; here the frame owns
    // that bookkeeping, and every branch nothing downstream awaited has to be joined by
    // hand or the frame reports done with work still running.
    co_await skinning;
    co_await replication;
    co_await stats;
    co_await gc;
}

// --- the frame runner ---------------------------------------------------------------

// Run `frames` frames of `variant` at `scale`; reports the measured average ms/frame, the
// serial budget at that scale, and entity 0's published transform (deterministic:
// propagation wrote local_xf + bodies = 2 + 3 = 5, in every variant).
void frame_stats(int frames, float scale, Frame_variant variant, double& avg_ms, double& serial_ms,
    float& transform0)
{
    constexpr int entities = 1000;
    time_scale = scale;
    reset_stats();

    World world{ entities };
    const bool graph_free = variant == Frame_variant::graph_free;
    ts::Static_task_graph graph;
    if (!graph_free)
        graph = build_frame_graph(world, variant);

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

        if (graph_free)
            run_frame_graph_free(world).sync();
        else
            graph.execute().sync();
    }
    double total_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();

    avg_ms = total_ms / frames;
    serial_ms = serial_budget_ms() * scale;
    transform0 = world.transforms.read([](const Transforms& t) { return t.size() > 0 ? t.get(0) : 0.0f; }).sync();
}

} // namespace

// --- entry points -----------------------------------------------------------------

// The graph baseline (see `frame_stats`).
void game_frame_stats(int frames, float scale, double& avg_ms, double& serial_ms, float& transform0)
{
    frame_stats(frames, scale, Frame_variant::baseline, avg_ms, serial_ms, transform0);
}

// The same frame with no `Static_task_graph` (see `run_frame_graph_free`): same World,
// same system bodies, hand-written schedule. Must produce the same `transform0`.
void game_frame_free_stats(int frames, float scale, double& avg_ms, double& serial_ms, float& transform0)
{
    frame_stats(frames, scale, Frame_variant::graph_free, avg_ms, serial_ms, transform0);
}

// Draw commands submitted over the last stats run. `submit` clears the queue, so this counts
// only what the three producers pushed BEFORE it ran -- an observable the producer/submit
// ordering decides. The transform invariant cannot see that ordering (every mock system
// writes the same constant every frame, so a one-frame skew is invisible), which is why the
// equivalence test compares this too.
long long game_frame_draw_count()
{
    return drawn.load();
}

// Compile the frame graph and write its structure as Graphviz DOT (no frames run).
void dump_game_frame_dot(const char* path)
{
    World world{ 8 };
    build_frame_graph(world, Frame_variant::baseline, path);
}

namespace
{

#if TS_PROFILING
// The SVG path for a described run: the description, spaces stripped, becomes a
// suffix before the extension ("baseline" -> "..._baseline.svg"). Only the traced
// path uses it, so it is compiled out with the trace (TS_PROFILING=0).
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
#endif

#if TS_PROFILING
// Convert a steady_clock tick count (the unit of `Scheduler::body_ticks()` etc.) to µs.
double ticks_to_us(long long ticks)
{
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::duration(ticks)).count();
}

// The worker-less ground-truth overhead for a variant. On a single-thread serial run the whole
// frame executes inline on one thread with no idle to confound it, so `total_wall - B` is the
// COMPLETE framework cost by pure subtraction (setup, dispatch, completion, pipe, trampoline --
// everything the summed-M accumulator may still miss on a multi-worker run). This is the oracle
// the multi-worker summed-M is measured against. Returns the complement `(total - B)/total`,
// which the caller attaches to the multi-worker trace for the dual print.
//
// B comes from the scheduler's body accumulator (fed here by the overflow lane, since a
// worker-less run has no workers). `total` is the wall of the whole `execute().sync()` frame
// loop -- not the trace's node-span makespan, which omits the pre-first-node setup and the fold.
// A trace is attached only to arm the body tracking (its fold is skipped for a worker-less run);
// no SVG is written. B comes from the scheduler's body accumulator (add-only), fed here by the
// overflow lane since a worker-less run has no workers; machinery is not accumulated -- the whole
// serial framework cost is the pure subtraction `total - B`, which is what makes this the oracle.
double serial_ground_truth(int frames, Frame_variant variant, const char* description)
{
    constexpr int entities = 1000;
    time_scale = 1.0f;
    reset_stats();

    ts::Scheduler_scope pool{ { .single_threaded = true } };
    World world{ entities };
    ts::Static_task_graph graph = build_frame_graph(world, variant, nullptr);

    ts::tools::Graph_trace trace;   // attach only to arm body tracking (fold is skipped)
    graph.set_trace(&trace);

    ts::Scheduler& sched = ts::global_scheduler();
    long long body0 = sched.body_ticks();
    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f)
        graph.execute().sync();
    auto t1 = std::chrono::steady_clock::now();
    long long body1 = sched.body_ticks();
    graph.set_trace(nullptr);

    double total_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    double body_us = ticks_to_us(body1 - body0);
    double per_run_total = total_us / frames;
    double per_run_body = body_us / frames;
    double complement = total_us > 0.0 ? (total_us - body_us) / total_us : 0.0;

    std::printf("[game_frame] %s worker-less ground truth: %.1f us/frame total, %.1f us body -> "
                "framework (total-B)/total = %.1f%%\n",
        description, per_run_total, per_run_body, 100.0 * complement);
    return complement;
}
#endif

// One traced variant on the current scheduler: build the variant's graph on a
// fresh World, attach a fresh trace, run, write the described SVG. `ground_truth` is the
// worker-less complement for this variant (negative = none), shown next to the summed-M
// overhead in the SVG headline and the console line.
void trace_variant(int frames, Frame_variant variant, const char* base_SVG_path, const char* description,
    const char* DOT_path, double ground_truth = -1.0)
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
    if (ground_truth >= 0.0)
        trace.set_ground_truth_overhead(ground_truth);
    graph.set_trace(&trace);
    for (int f = 0; f < frames; ++f)
        graph.execute().sync();
    graph.set_trace(nullptr);
    std::string path = described_SVG_path(base_SVG_path, description);
    trace.write_SVG(path.c_str());
    std::printf("[game_frame] %s: traced %lld runs -> %s\n", description, trace.run_count(), path.c_str());
    // Dual print: the multi-worker overhead (machinery M = busy - B, pure subtraction) side by
    // side with the worker-less serial floor and their gap. The multi-worker figure sits ABOVE
    // the floor; the gap is the machinery only workers pay (cross-thread dispatch, pipe hand-off,
    // park/wake) -- the price of the parallelism, not a measurement error.
    double ov = trace.overhead();
    std::printf("[game_frame] %s: task-system overhead (M = busy - B) = %.1f%% (body %.1f / machinery %.1f us per run)",
        description, 100.0 * ov, trace.body_us(), trace.machinery_us());
    if (ground_truth >= 0.0)
        std::printf("; worker-less serial floor = %.1f%%; gap +%.2f pt (parallelization tax above the floor)",
            100.0 * ground_truth, 100.0 * (ov - ground_truth));
    std::printf("\n");
    // Four-way subtraction breakdown: every worker-moment of core-time T = workers x makespan is
    // body, machinery (M = busy - B), or idle (T - busy); the per-run top-level execute() setup is
    // the off-worker orchestration bucket on top.
    std::printf("[game_frame] %s: four-way (of core-time) body %.1f%% | machinery(M = busy - B) %.1f%% | "
                "idle %.1f%% | orchestration %.1f%% (%.1f us/frame)\n",
        description, 100.0 * trace.four_way_body_share(), 100.0 * trace.four_way_machinery_share(),
        100.0 * trace.four_way_idle_share(), 100.0 * trace.four_way_orchestration_share(),
        trace.four_way_orchestration_us());
#else
    for (int f = 0; f < frames; ++f)
        graph.execute().sync();
    (void)base_SVG_path; (void)description; (void)ground_truth;
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
#if TS_PROFILING
    // Worker-less ground truth first (its own single-thread scheduler per variant): the serial
    // (total-B)/total oracle, attached to the multi-worker trace below for the dual print.
    double gt_baseline = serial_ground_truth(frames, Frame_variant::baseline, "baseline");
    double gt_optimised = serial_ground_truth(frames, Frame_variant::optimised, "optimised");
#else
    double gt_baseline = -1.0, gt_optimised = -1.0;
#endif
    ts::Scheduler_scope pool{ { .num_threads = static_cast<uint32_t>(variant_workers) } };
    trace_variant(frames, Frame_variant::baseline, SVG_path, "baseline", DOT_path, gt_baseline);
    trace_variant(frames, Frame_variant::optimised, SVG_path, "optimised", nullptr, gt_optimised);
}

// Headless run of the OPTIMISED variant on a dedicated `workers`-thread
// scheduler, at a fast scale -- for the sanitizer driver to exercise the
// concurrent surface unique to that variant: the gameplay `Versioned` publisher
// (AI reads the previous version while a flip publishes the next), the
// `Deferred` draw-queue staging, and the explicit submit ordering. The baseline
// `game_frame_stats` path exercises none of these.
void stress_game_frame_optimised(int frames, int workers)
{
    constexpr int entities = 500;
    time_scale = 0.05f;
    reset_stats();
    ts::Scheduler_scope pool{ { .num_threads = static_cast<uint32_t>(workers) } };
    World world{ entities };
    ts::Static_task_graph graph = build_frame_graph(world, Frame_variant::optimised);
    for (int f = 0; f < frames; ++f)
        graph.execute().sync();
}

void run_game_frame_sample(int frames, float scale)
{
    double avg_ms = 0.0, serial_ms = 0.0;
    float transform0 = 0.0f;
    game_frame_stats(frames, scale, avg_ms, serial_ms, transform0);
    long long graph_drawn = game_frame_draw_count();

    // The same frame with no graph, for the side-by-side (`--bench` reports it properly;
    // this line keeps the sample and the stress loop exercising the path).
    double free_ms = 0.0, free_serial_ms = 0.0;
    float free_transform0 = 0.0f;
    game_frame_free_stats(frames, scale, free_ms, free_serial_ms, free_transform0);
    long long free_drawn = game_frame_draw_count();

    int workers = ts::global_scheduler().worker_count();
    double ideal_ms = serial_ms / workers;
    std::printf("\n[game_frame] %d frames, 1000 entities, scale %.2f\n", frames, scale);
    std::printf("  %.2f ms/frame; %.2f ms of work across %d workers -> ideal %.2f ms/frame, "
                "%.0f%% utilization\n",
        avg_ms, serial_ms, workers, ideal_ms, 100.0 * ideal_ms / avg_ms);
    std::printf("  peak %d concurrent nav queries, %d early-outed on cancel\n",
        nav_peak.load(), nav_early.load());
    std::printf("  streamed %d assets, %d batches via a fire-and-forget coroutine\n",
        streamed.load(), batches.load());
    std::printf("  %lld draw commands staged/written by producers, applied by submit\n",
        graph_drawn);
    std::printf("  %d HUD snapshots via multi-object ts::async (combat + economy)\n",
        hud_snapshots.load());
    std::printf("  graph-free composition of the same frame: %.2f ms/frame (%+.1f%%), "
                "transform0 %.1f, %lld draw commands\n",
        free_ms, 100.0 * (free_ms - avg_ms) / avg_ms, free_transform0, free_drawn);
}

} // namespace sample
