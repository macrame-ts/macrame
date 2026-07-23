// A mock game-engine frame -- the breadth sample. Eighteen systems and a publish
// node; the schedule is derived from declared data access. The only explicit
// edges are the grant-free orderings (called out below), where staging leaves
// no conflict to derive from.
//
// What it shows, layer by layer:
//   - `Static_task_graph` -- nodes over guarded stores; every edge derived
//     from parameter const-ness (fn-pointer nodes, lambda nodes, and one bare
//     generic lambda -- `const auto&` deduces a read via the rvalue probe).
//   - `Versioned<Transforms>` -- the packaged double-buffer. Early systems
//     (gameplay/nav/AI/audio) read last frame's published transforms all
//     frame; the propagation node stages this frame's batch grant-free; a
//     publish node flips; late systems (cloth/culling/particles/render) read
//     the fresh version. Which side of the flip a reader declares is a
//     scheduling lever (see audio in `build_frame_graph`).
//   - `Deferred<Draw_lists>` -- the command buffer. Culling, particles, and UI
//     each stage draw commands grant-free (no contention among producers, none
//     with the queue's owner); the submit node applies the whole batch as one
//     write. The draw queue is the classic engine use of this shape.
//   - Dynamic work outside the graph: streaming fires `async` loads consumed by
//     `then` continuations and joined by `when_all`; AI fires speculative,
//     cancellable `async` queries against the nav service (body-level early-out
//     via a trailing `Cancellation_token`).
//   - Internal parallelism: heavy systems split their work with
//     `ts::parallel_for` under the node's access grant.
//
// Two ideas this sample is built to show (shared with the physics sample):
//   - The core logic is plainly thread-unsafe, and that's the point. Every
//     store and system below is a clean single-threaded class or function -- no
//     atomics, no locks, only domain logic. The library is what turns them into
//     safely parallelisable work; they never learn about threading.
//   - Parallelism follows from separation. Splitting the frame into
//     single-writer stores (three gameplay systems instead of one monolith,
//     transforms into a published version, draws into a staged queue) is honest
//     parallelisation work -- but the split is what creates the wide, safe DAG,
//     and the library's access declarations make it explicit and checkable
//     rather than implicit convention.

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
#include <thread>
#include <tuple>
#include <vector>

namespace sample
{

namespace
{

// Scales every system's mock cost. 1.0 = real AAA @ 60 fps budgets; tests and
// the stress loop use a small scale so many frames run fast.
float time_scale = 1.0f;

// --- instrumentation --------------------------------------------------------------

// `Guarded::async` demo: concurrent nav queries in flight / peak, and queries
// that early-outed on the cancel token mid-body.
std::atomic<int> nav_active{ 0 };
std::atomic<int> nav_peak{ 0 };
std::atomic<int> nav_early{ 0 };
// Streaming demo: assets processed via `then`, batches joined via `when_all`.
std::atomic<int> streamed{ 0 };
std::atomic<int> batches{ 0 };
// Deferred demo: draw commands applied by the submit node.
std::atomic<long long> drawn{ 0 };

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
}

// Mock a system's CPU cost: spin-wait for the budget. Precise (unlike
// `std::this_thread::sleep_for`, which rounds sub-ms waits up to the ~15 ms
// Windows timer tick) and, like real compute, it occupies the worker for the
// duration so graph parallelism is measurable.
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
// whole batch (idempotent), so the replay resync re-applies deterministically
// and both replicas stay bit-identical.
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

// The render queue -- the `Deferred` target. Producers never touch it directly:
// they stage commands; the submit node applies them under its write grant.
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
using Velocities = Float_store;
using Asset_source = Float_store;
using Input = Float_store;
using Net = Float_store;
using Assets = Float_store;
using Combat = Float_store;
using Economy = Float_store;
using Quests = Float_store;
using Paths = Float_store;
using Intents = Float_store;
using Local_xf = Float_store;
using Bodies = Float_store;
using Cloth = Float_store;
using Visibility = Float_store;
using Particles = Float_store;
using Audio_out = Float_store;
using Ui = Float_store;

// --- the world --------------------------------------------------------------------

// Every mutable store has a single writer system, so the frame graph derives a
// clean DAG from the declarations. Transforms are `Versioned` (readers declared
// before the publish node see last frame's version, readers after it see this
// frame's); the draw queue is `Deferred` (producers stage, submit applies).
// Destruction order matters: the graph (declared later, destroyed first) holds
// the recorders; then `draw_staged` verifies nothing staged was lost; then the
// stores.
struct World
{
    explicit World(int n)
        : skeletons{ ts::Named{"skeletons"}, n }, nav_mesh{ ts::Named{"nav_mesh"}, n }
        , renderables{ ts::Named{"renderables"}, n }, velocities{ ts::Named{"velocities"}, n }
        , asset_source{ ts::Named{"asset_source"}, n }
        , input{ ts::Named{"input"}, n }, net{ ts::Named{"net"}, n }
        , assets{ ts::Named{"assets"}, n }, combat{ ts::Named{"combat"}, n }
        , economy{ ts::Named{"economy"}, n }, quests{ ts::Named{"quests"}, n }
        , paths{ ts::Named{"paths"}, n }, intents{ ts::Named{"intents"}, n }
        , local_xf{ ts::Named{"local_xf"}, n }, bodies{ ts::Named{"bodies"}, n }
        , cloth{ ts::Named{"cloth"}, n }, visibility{ ts::Named{"visibility"}, n }
        , particles{ ts::Named{"particles"}, n }, audio_out{ ts::Named{"audio_out"}, n }
        , ui{ ts::Named{"ui"}, n }
    {}

    // read-only static inputs (no system writes them this frame)
    ts::Guarded<Skeletons> skeletons;
    ts::Guarded<Nav_mesh> nav_mesh;          // Navigation reads the store; AI queries the service
    ts::Guarded<Renderables> renderables;
    ts::Guarded<Velocities> velocities;
    ts::Guarded<Asset_source> asset_source;  // Streaming loads from this

    // single-writer outputs (writer named by the type)
    ts::Guarded<Input> input;
    ts::Guarded<Net> net;
    ts::Guarded<Assets> assets;
    ts::Guarded<Combat> combat;
    ts::Guarded<Economy> economy;
    ts::Guarded<Quests> quests;
    ts::Guarded<Paths> paths;
    ts::Guarded<Intents> intents;
    ts::Guarded<Local_xf> local_xf;
    ts::Guarded<Bodies> bodies;

    // the published transforms: staged by propagation, flipped by the publish node
    ts::Versioned<Transforms> transforms{ ts::Named{"transforms"} };

    ts::Guarded<Cloth> cloth;
    ts::Guarded<Visibility> visibility;
    ts::Guarded<Particles> particles;
    ts::Guarded<Audio_out> audio_out;
    ts::Guarded<Ui> ui;

    // the render queue: culling/particles/UI stage into it, submit applies
    ts::Guarded<Draw_lists> draw_lists{ ts::Named{"draw_lists"} };
    ts::Deferred<Draw_lists> draw_staged{ draw_lists };
};

// --- shared helpers ---------------------------------------------------------------

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

void read_all(const Transforms& t)
{
    volatile float sink = 0;
    for (int i = 0, n = t.size(); i < n; ++i)
        sink = t.get(i);
    (void)sink;
}

// Internally-parallel systems run their work through `ts::parallel_for`, under
// the calling node's access grant (the node holds exclusive access to its
// output; slices are disjoint by construction).
//
// The manual slicing below is an artifact of MOCKING the cost: the real
// per-item work (`set`) is trivial, and the simulated cost is a `spin`, which
// cannot be divided per item (a microsecond-scale spin per element is mostly
// `steady_clock` reads, distorting the budget). So the spin is cut into a few
// coarse slices and the item range is partitioned along with it. Real code with
// real per-item cost would iterate items directly --
// `ts::parallel_for(entity_count, per_entity_fn)` -- and let the library's
// balance modes distribute the load.
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

// Cost-only variant: the system's real writes are elsewhere (or trivial), but
// its simulated compute is internally parallel.
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

void tick_input(Input& input)                                                     // 0.1
{
    fill(input, 1.0f);
    spin(0.1);
}

void tick_networking(const Input& input, Net& net)                                // 0.5
{
    read_all(input);
    fill(net, 1.0f);
    spin(0.5);
}

// Streaming: loads a batch of assets from the read-only source via
// `Guarded::async` -- `async`, not `access`: the loads must run on other
// workers, overlapping this node's own cost below. Each load is consumed by a
// `then` continuation; a `when_all` joins the batch. Fire-and-forget: the
// handles are dropped, the chain stays alive through the queued work.
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

    spin(1.5);   // the node's own cost (decompression), overlapping the async loads
}

// Gameplay is three systems, not one monolith: they share inputs (all read last
// frame's transforms, input, net) but own disjoint outputs, so they run in
// parallel. Splitting the coarse "gameplay" store is where the frame's serial
// spine shortens -- granularity decides parallelism.

void tick_combat(const Transforms& prev_xf, const Input& input,
                 const Net& net, Combat& combat)                                  // 0.7
{
    read_all(prev_xf);
    read_all(input);
    read_all(net);
    fill(combat, 1.0f);
    spin(0.7);
}

void tick_economy(const Transforms& prev_xf, const Input& input,
                  const Net& net, Economy& economy)                               // 0.7
{
    read_all(prev_xf);
    read_all(input);
    read_all(net);
    fill(economy, 1.0f);
    spin(0.7);
}

void tick_quests(const Transforms& prev_xf, const Input& input,
                 const Net& net, Quests& quests)                                  // 0.7
{
    read_all(prev_xf);
    read_all(input);
    read_all(net);
    fill(quests, 1.0f);
    spin(0.7);
}

void tick_navigation(const Nav_mesh& nav_mesh, const Transforms& prev_xf,
                     Paths& paths)                                                // 2.5
{
    read_all(nav_mesh);
    read_all(prev_xf);
    parallel_fill(paths, 1.0f, 2.5);      // internal parallelism: per-query batch
}

// AI: per-agent path queries against the read-only nav service via
// `Guarded::async` -- concurrent readers on the service, on other workers,
// overlapping AI's own logic. Fire-and-forget (consuming a result would mean
// blocking inside a graph node -- the anti-pattern; feeding results into a
// downstream node is the clean shape). The queries are speculative: each runs a
// longer budget than AI needs, and AI cancels the batch once its own work is
// done. The query body opts into cooperative cancellation by taking a trailing
// `Cancellation_token` and polling it -- cancellation arriving mid-body
// early-outs (a cooperative return settles the task completed, not cancelled).
// AI's own evaluation is per-agent and internally parallel, like the other
// heavy systems.
void tick_ai(ts::Guarded<Nav_mesh>& nav_service,
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
                if (tok.is_cancel_requested()) { bailed = true; break; }   // early-out
                std::this_thread::yield();
            }
            float v = n.size() > 0 ? n.get(0) : 0.0f;
            nav_active.fetch_sub(1);
            if (bailed)
                nav_early.fetch_add(1, std::memory_order_relaxed);
            return v;
        }, { .token = nav_cancel.token() });

    parallel_cost(1.5);            // per-agent evaluation, overlapping the queries
    nav_cancel.request_cancel();   // AI has what it needs -> stragglers early-out
    fill(intents, 1.0f);
}

void tick_animation(const Skeletons& skeletons, const Intents& intents,
                    Local_xf& local_xf)                                           // 3.0
{
    read_all(skeletons);
    read_all(intents);
    parallel_fill(local_xf, 2.0f, 3.0);   // internal parallelism: per-skeleton
}

void tick_physics(const Velocities& velocities, const Combat& combat,
                  Bodies& bodies)                                                 // 4.5
{
    read_all(velocities);
    read_all(combat);
    parallel_fill(bodies, 3.0f, 4.5);     // internal parallelism: per-island
}

void tick_cloth(const Transforms& xf, Cloth& cloth)                               // 2.0
{
    read_all(xf);
    parallel_fill(cloth, 1.0f, 2.0);      // internal parallelism: per-patch
}

// Culling produces the visible set and stages its draw batch -- no grant on the
// draw queue, so the three draw producers never contend with each other or with
// the submit node's other inputs.
void tick_culling(const Transforms& xf, const Renderables& renderables,
                  Visibility& visibility, ts::Recorder<Draw_lists>& draws)        // 1.5
{
    read_all(xf);
    read_all(renderables);
    parallel_fill(visibility, 1.0f, 1.5);   // internal parallelism: per-view
    draws.stage([n = visibility.size()](Draw_lists& d) { d.push_batch(n); });
}

void tick_particles(const Transforms& xf, Particles& particles,
                    ts::Recorder<Draw_lists>& draws)                              // 2.0
{
    read_all(xf);
    parallel_fill(particles, 1.0f, 2.0);    // internal parallelism: per-system
    draws.stage([n = particles.size() / 4](Draw_lists& d) { d.push_batch(n); });
}

// Audio mixes off LAST frame's transforms (its node is declared before the
// flip): one frame of positional latency is inaudible, and reading the previous
// version takes the serial mixer out of the post-flip tail -- the same
// version-choice lever the gameplay systems use, applied to shorten the
// critical path.
void tick_audio(const Transforms& prev_xf, Audio_out& audio_out)                  // 1.5
{
    read_all(prev_xf);
    fill(audio_out, 1.0f);
    spin(1.5);   // deliberately serial: a single-threaded mixer is realistic
}

void tick_ui(const Quests& quests, Ui& ui, ts::Recorder<Draw_lists>& draws)       // 0.5
{
    read_all(quests);
    fill(ui, 1.0f);
    spin(0.5);
    draws.stage([n = ui.size() / 10](Draw_lists& d) { d.push_batch(n); });
}

// Submit: applies the staged draw batches as one write (`commit` under the
// grant this node already holds), consumes the queue, clears it for the next
// frame. One acquisition amortized over every producer's commands.
void tick_submit(ts::Deferred<Draw_lists>& staged,
                 const Transforms& xf, Draw_lists& draw_lists)                    // 2.5
{
    read_all(xf);
    staged.commit(draw_lists);
    drawn.fetch_add(draw_lists.count(), std::memory_order_relaxed);
    parallel_cost(2.5);                     // internal parallelism: per-pass
    draw_lists.clear();
}

// Sum of the budgets above (ms @ 1.0). The metric that matters is resource
// utilization: `serial_budget / workers` is the perfect-utilization lower bound
// on ms/frame, and measured-vs-ideal says how well the cores were used. The
// achievable floor is `max(serial / workers, critical path)` -- the longest
// dependency chain through the frame bounds it once workers are plentiful.
double serial_budget_ms()
{
    return 0.1 + 0.5 + 1.5 + 0.7 + 0.7 + 0.7 + 2.5 + 1.5 + 3.0 + 4.5 + 1.0
         + 2.0 + 1.5 + 2.0 + 1.5 + 2.5 + 0.5 + 0.2;
}

// --- the frame graph --------------------------------------------------------------

// Nodes are added in frame order, so the conflict tiebreak (declaration index)
// matches intent: everything before the publish node reads last frame's
// transforms, everything after it reads this frame's. The explicit edges are
// exactly the grant-free orderings: `flip.after(propagation)` and
// `submit.after(<the three draw producers>)` -- staging holds no grant on its
// target, so there is no conflict edge to derive; the ordering is intent, and
// is declared as such.
ts::Static_task_graph build_frame_graph(World& w, const char* dot_path = nullptr)
{
    ts::Static_task_graph g;

    // Node priorities are deliberately left at default. A hand optimization
    // pass (see docs/TODO.md, profiler-guided optimization) tried rank
    // priorities and prev-transform placements for cloth/particles; no
    // configuration beat plain greedy dispatch beyond measurement noise, and
    // two mechanisms argue against naive ranking: `low` is valve-gated
    // background (a mislabeled dependency stalls its dependents), and `high`
    // dispatches through the global queues rather than the per-worker deques.
    // The simplest configuration wins until measured evidence says otherwise.
    g.add_node("input", &tick_input, w.input);
    g.add_node("networking", &tick_networking, w.input, w.net);
    // Streaming and AI capture the service wrappers (they submit async work);
    // their graph access is still just the declared stores.
    g.add_node("streaming", [&w](const Input& input, Assets& assets)
    {
        tick_streaming(w.asset_source, input, assets);
    }, w.input, w.assets);
    g.add_node("combat", &tick_combat, w.transforms.state(), w.input, w.net, w.combat);
    g.add_node("economy", &tick_economy, w.transforms.state(), w.input, w.net, w.economy);
    g.add_node("quests", &tick_quests, w.transforms.state(), w.input, w.net, w.quests);
    g.add_node("navigation", &tick_navigation, w.nav_mesh, w.transforms.state(), w.paths);
    // Audio reads last frame's transforms (declared before the flip): one frame
    // of positional latency is inaudible, and the serial mixer runs from t=0
    // instead of the post-flip tail. Cloth/particles could make the same
    // staleness trade to fill the frame's idle first half; a hand pass tried it
    // and found no win beyond measurement noise -- a plausible mechanism being
    // that filler slices delay the serial spine (a ready node waits for a
    // worker to finish its current slice; nothing evicts a runner). Notes in
    // docs/TODO.md under profiler-guided optimization; they stay post-flip.
    g.add_node("audio", &tick_audio, w.transforms.state(), w.audio_out);
    g.add_node("ai", [&w](const Transforms& prev_xf, const Paths& paths, const Combat& combat,
                          const Economy& economy, const Quests& quests, Intents& intents)
    {
        tick_ai(w.nav_mesh, prev_xf, paths, combat, economy, quests, intents);
    }, w.transforms.state(), w.paths, w.combat, w.economy, w.quests, w.intents);
    g.add_node("animation", &tick_animation, w.skeletons, w.intents, w.local_xf);
    g.add_node("physics", &tick_physics, w.velocities, w.combat, w.bodies);

    // Propagation: computes this frame's transforms from animation + physics
    // output and stages the batch -- one command, cheap to replay. No grant on
    // the transforms; it never contends with their readers.
    auto propagation = g.add_node("propagation",
        [rec = w.transforms.recorder()](const Local_xf& local_xf, const Bodies& bodies) mutable
        {
            std::vector<float> out(static_cast<std::size_t>(local_xf.size()));
            for (int i = 0, n = local_xf.size(); i < n; ++i)
                out[static_cast<std::size_t>(i)] = local_xf.get(i) + bodies.get(i);
            parallel_cost(1.0);   // internal parallelism: per-subtree
            rec.stage([batch = std::move(out)](Transforms& t) { t.apply(batch); });
        },
        w.local_xf, w.bodies);
    propagation;

    auto flip = g.add_node("flip", ts::publish_body(w.transforms), w.transforms.state());
    flip.after(propagation);

    // Post-flip readers of the fresh version. The draw producers' recorders are
    // minted in declaration order (cloth has none; culling, particles, ui) --
    // the apply order at commit is recorder-creation order, fixed at build
    // time, independent of thread timing.
    g.add_node("cloth", &tick_cloth, w.transforms.state(), w.cloth);
    auto culling = g.add_node("culling",
        [rec = w.draw_staged.recorder()](const Transforms& xf, const Renderables& r,
                                         Visibility& v) mutable
        {
            tick_culling(xf, r, v, rec);
        }, w.transforms.state(), w.renderables, w.visibility);
    culling;
    auto particles = g.add_node("particles",
        [rec = w.draw_staged.recorder()](const Transforms& xf, Particles& p) mutable
        {
            tick_particles(xf, p, rec);
        }, w.transforms.state(), w.particles);
    particles;
    g.add_node("debug_overlay", [](const auto& economy, const auto& xf)   // 0.2;
    {                                                    // generic lambda: `const auto&`
        read_all(economy);                               // deduces a read via the probe
        read_all(xf);
        spin(0.2);
    }, w.economy, w.transforms.state());
    auto ui = g.add_node("ui",
        [rec = w.draw_staged.recorder()](const Quests& quests, Ui& u) mutable
        {
            tick_ui(quests, u, rec);
        }, w.quests, w.ui);
    ui;

    auto submit = g.add_node("submit", [&w](const Transforms& xf, Draw_lists& dl)
    {
        tick_submit(w.draw_staged, xf, dl);
    }, w.transforms.state(), w.draw_lists);
    submit;
    submit.after(culling).after(particles).after(ui);

    g.compile(dot_path);
    return g;
}

} // namespace

// --- entry points -----------------------------------------------------------------

// Run `frames` frames at `scale`; returns the measured average ms/frame, the
// serial budget at that scale, and entity 0's published transform (a
// deterministic output: propagation wrote local_xf + bodies = 2 + 3).
void game_frame_stats(int frames, float scale,
                      double& avg_ms, double& serial_ms, float& transform0)
{
    constexpr int entities = 1000;
    time_scale = scale;
    reset_stats();

    // The world is created and destroyed in here, so all fire-and-forget async
    // work drains too (a return == no deadlock, no lost staged work).
    World world{ entities };
    ts::Static_task_graph graph = build_frame_graph(world);

    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    for (int f = 0; f < frames; ++f)
        graph.execute().sync();
    double total_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();

    avg_ms = total_ms / frames;
    serial_ms = serial_budget_ms() * scale;
    transform0 = world.transforms.read([](const Transforms& t)
    {
        return t.size() > 0 ? t.get(0) : 0.0f;
    }).sync();
}

// Compile the frame graph and write its structure as Graphviz DOT (no frames run; the
// structure does not depend on entity count).
void dump_game_frame_dot(const char* path)
{
    World world{ 8 };
    build_frame_graph(world, path);
}

// Run `frames` frames with an aggregating `Graph_trace` attached; writes the average-run
// timeline SVG plus the structure DOT (so the pair stays in sync). With `TS_PROFILING`
// 0 the frames still run but nothing is traced.
void trace_game_frame(int frames, const char* dot_path, const char* svg_path)
{
    constexpr int entities = 1000;
    time_scale = 1.0f;
    reset_stats();

    World world{ entities };
    ts::Static_task_graph graph = build_frame_graph(world, dot_path);
#if TS_PROFILING
    ts::tools::Graph_trace trace;
    graph.set_trace(&trace);
    for (int f = 0; f < frames; ++f)
        graph.execute().sync();
    graph.set_trace(nullptr);   // the trace is scoped inside this function; detach before it dies
    trace.write_svg(svg_path);
    std::printf("[game_frame] traced %lld runs -> %s (structure: %s)\n",
        trace.run_count(), svg_path, dot_path);
#else
    for (int f = 0; f < frames; ++f)
        graph.execute().sync();
    (void)svg_path;
    std::printf("[game_frame] TS_PROFILING is 0: ran %d frames, no trace written\n", frames);
#endif
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
    std::printf("  peak %d concurrent nav queries (Guarded::async during AI), %d early-outed on cancel\n",
        nav_peak.load(), nav_early.load());
    std::printf("  streamed %d assets via then, %d batches via when_all\n",
        streamed.load(), batches.load());
    std::printf("  %lld draw commands staged by 3 producers, applied by submit via Deferred\n",
        drawn.load());
}

} // namespace sample
