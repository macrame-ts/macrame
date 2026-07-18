// A mock game-engine frame -- the breadth sample. Sixteen systems and a publish
// node; the whole schedule is derived from declared data access (zero explicit
// ordering edges except one, called out below).
//
// What it shows, layer by layer:
//   - `Static_task_graph` -- 17 nodes over 19 guarded stores; every edge derived
//     from parameter const-ness (fn-pointer nodes, lambda nodes, and one bare
//     generic lambda -- `const auto&` deduces a read via the rvalue probe).
//   - `Versioned<Transforms>` -- the packaged double-buffer. Early systems
//     (gameplay/nav/AI) read last frame's published transforms all frame; the
//     propagation node stages this frame's batch grant-free; a publish node
//     flips; late systems (cloth/culling/particles/audio/render) read the fresh
//     version.
//   - Dynamic work outside the graph: streaming fires `async` loads consumed by
//     `then` continuations and joined by `when_all`; AI fires speculative,
//     cancellable `async` queries against the nav service (body-level early-out
//     via a trailing `Cancellation_token`).
//   - Internal parallelism: the heavy systems split their work with
//     `ts::parallel_for` under the node's access grant.
//
// Two ideas this sample is built to show (shared with the physics sample):
//   - The core logic is plainly thread-unsafe, and that's the point. Every
//     store and system below is a clean single-threaded class or function -- no
//     atomics, no locks, only domain logic. The library is what turns them into
//     safely parallelisable work; they never learn about threading.
//   - Parallelism follows from separation. Splitting the frame into
//     single-writer stores (and transforms into a published version) is honest
//     parallelisation work -- but the split is what creates the wide, safe DAG,
//     and the library's access declarations make it explicit and checkable
//     rather than implicit convention.

#include "ts/access.h"
#include "ts/guarded.h"
#include "ts/parallel_for.h"
#include "ts/static_task_graph.h"
#include "ts/task.h"
#include "ts/versioned.h"

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

// --- the world --------------------------------------------------------------------

// Every mutable store has a single writer system, so the frame graph derives a
// clean DAG from the declarations. Transforms are `Versioned`: readers declared
// before the publish node see last frame's version, readers after it see this
// frame's -- no reader ever waits on the writer.
struct World
{
    explicit World(int n)
        : skeletons{ n }, nav_mesh{ n }, renderables{ n }, velocities{ n }, asset_source{ n }
        , input{ n }, net{ n }, assets{ n }, game_state{ n }, paths{ n }, intents{ n }
        , local_xf{ n }, bodies{ n }
        , cloth{ n }, visibility{ n }, particles{ n }, audio_out{ n }, draw_lists{ n }, ui{ n }
    {}

    // read-only static inputs (no system writes them this frame)
    ts::Guarded<Float_store> skeletons;
    ts::Guarded<Float_store> nav_mesh;       // Navigation reads the store; AI queries the service
    ts::Guarded<Float_store> renderables;
    ts::Guarded<Float_store> velocities;
    ts::Guarded<Float_store> asset_source;   // Streaming loads from this

    // single-writer outputs
    ts::Guarded<Float_store> input;          // Input
    ts::Guarded<Float_store> net;            // Networking
    ts::Guarded<Float_store> assets;         // Streaming
    ts::Guarded<Float_store> game_state;     // Gameplay
    ts::Guarded<Float_store> paths;          // Navigation
    ts::Guarded<Float_store> intents;        // AI
    ts::Guarded<Float_store> local_xf;       // Animation
    ts::Guarded<Float_store> bodies;         // Physics

    // the published transforms: staged by propagation, flipped by the publish node
    ts::Versioned<Transforms> transforms;

    ts::Guarded<Float_store> cloth;          // Cloth
    ts::Guarded<Float_store> visibility;     // Culling
    ts::Guarded<Float_store> particles;      // Particles
    ts::Guarded<Float_store> audio_out;      // Audio
    ts::Guarded<Float_store> draw_lists;     // Render
    ts::Guarded<Float_store> ui;             // UI
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

// Heavy systems split their per-entity work across chunks with the core
// `ts::parallel_for`; chunks run under the calling node's access grant (the
// node holds exclusive access to `out`, and the chunks are disjoint by
// construction). The cost spin is divided too, so wall time ~ total / chunks.
constexpr int parallel_chunks = 8;

void parallel_fill(Float_store& out, float v, double total_ms)
{
    int n = out.size();
    ts::parallel_for(parallel_chunks, [&out, v, total_ms, n](int c)
    {
        int begin = c * n / parallel_chunks;
        int end = (c + 1) * n / parallel_chunks;
        for (int i = begin; i < end; ++i)
            out.set(i, v);
        spin(total_ms / parallel_chunks);
    });
}

// --- the systems ------------------------------------------------------------------
// Free functions with typed parameters: the const-ness is the access
// declaration the graph reads. Budgets (ms @ scale 1.0) in comments.

void tick_input(Float_store& input)                                               // 0.1
{
    fill(input, 1.0f);
    spin(0.1);
}

void tick_networking(const Float_store& input, Float_store& net)                  // 0.5
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
void tick_streaming(ts::Guarded<Float_store>& asset_source,
                    const Float_store& input, Float_store& assets)                // 0.5
{
    read_all(input);
    fill(assets, 1.0f);

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

void tick_gameplay(const Transforms& prev_xf, const Float_store& input,
                   const Float_store& net, Float_store& game_state)               // 2.0
{
    read_all(prev_xf);
    read_all(input);
    read_all(net);
    fill(game_state, 1.0f);
    spin(2.0);
}

void tick_navigation(const Float_store& nav_mesh, const Transforms& prev_xf,
                     Float_store& paths)                                          // 1.0
{
    read_all(nav_mesh);
    read_all(prev_xf);
    fill(paths, 1.0f);
    spin(1.0);
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
void tick_ai(ts::Guarded<Float_store>& nav_service,
             const Transforms& prev_xf, const Float_store& paths,
             const Float_store& game_state, Float_store& intents)                 // 1.5
{
    read_all(prev_xf);
    read_all(paths);
    read_all(game_state);

    ts::Cancellation_source nav_cancel;
    constexpr int queries = 6;
    for (int q = 0; q < queries; ++q)
        nav_service.async([](const Float_store& n, ts::Cancellation_token tok)
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

    spin(1.5);                     // AI's own logic, overlapping the queries
    nav_cancel.request_cancel();   // AI has what it needs -> stragglers early-out
    fill(intents, 1.0f);
}

void tick_animation(const Float_store& skeletons, const Float_store& intents,
                    Float_store& local_xf)                                        // 3.0
{
    read_all(skeletons);
    read_all(intents);
    parallel_fill(local_xf, 2.0f, 3.0);   // internal parallelism: per-skeleton
}

void tick_physics(const Float_store& velocities, const Float_store& game_state,
                  Float_store& bodies)                                            // 3.0
{
    read_all(velocities);
    read_all(game_state);
    parallel_fill(bodies, 3.0f, 3.0);     // internal parallelism: per-island
}

void tick_cloth(const Transforms& xf, Float_store& cloth)                         // 1.0
{
    read_all(xf);
    fill(cloth, 1.0f);
    spin(1.0);
}

void tick_culling(const Transforms& xf, const Float_store& renderables,
                  Float_store& visibility)                                        // 1.5
{
    read_all(xf);
    read_all(renderables);
    parallel_fill(visibility, 1.0f, 1.5);   // internal parallelism: per-view
}

void tick_particles(const Transforms& xf, Float_store& particles)                 // 1.5
{
    read_all(xf);
    parallel_fill(particles, 1.0f, 1.5);    // internal parallelism: per-system
}

void tick_audio(const Transforms& xf, Float_store& audio_out)                     // 0.5
{
    read_all(xf);
    fill(audio_out, 1.0f);
    spin(0.5);
}

void tick_render(const Transforms& xf, const Float_store& visibility,
                 const Float_store& particles, Float_store& draw_lists)           // 2.5
{
    read_all(xf);
    read_all(visibility);
    read_all(particles);
    parallel_fill(draw_lists, 1.0f, 2.5);   // internal parallelism: per-pass
}

void tick_ui(const Float_store& game_state, Float_store& ui)                      // 0.5
{
    read_all(game_state);
    fill(ui, 1.0f);
    spin(0.5);
}

// Sum of the budgets above (ms @ 1.0) -- the serial baseline to beat.
double serial_budget_ms()
{
    return 0.1 + 0.5 + 0.5 + 2.0 + 1.0 + 1.5 + 3.0 + 3.0 + 1.0
         + 1.0 + 1.5 + 1.5 + 0.5 + 2.5 + 0.5 + 0.2;
}

// --- the frame graph --------------------------------------------------------------

// Nodes are added in frame order, so the conflict tiebreak (declaration index)
// matches intent: everything before the publish node reads last frame's
// transforms, everything after it reads this frame's. The one explicit edge is
// `flip.after(propagation)` -- propagation holds no grant on the transforms
// (staging is grant-free), so no conflict edge exists to derive; the ordering
// is intent, and is declared as such.
ts::Static_task_graph build_frame_graph(World& w)
{
    ts::Static_task_graph g;

    g.add_node(&tick_input, w.input);
    g.add_node(&tick_networking, w.input, w.net);
    // Streaming and AI capture the service wrappers (they submit async work);
    // their graph access is still just the declared stores.
    g.add_node([&w](const Float_store& input, Float_store& assets)
    {
        tick_streaming(w.asset_source, input, assets);
    }, w.input, w.assets);
    g.add_node(&tick_gameplay, w.transforms.state(), w.input, w.net, w.game_state);
    g.add_node(&tick_navigation, w.nav_mesh, w.transforms.state(), w.paths);
    g.add_node([&w](const Transforms& prev_xf, const Float_store& paths,
                    const Float_store& game_state, Float_store& intents)
    {
        tick_ai(w.nav_mesh, prev_xf, paths, game_state, intents);
    }, w.transforms.state(), w.paths, w.game_state, w.intents);
    g.add_node(&tick_animation, w.skeletons, w.intents, w.local_xf);
    g.add_node(&tick_physics, w.velocities, w.game_state, w.bodies);

    // Propagation: computes this frame's transforms from animation + physics
    // output and stages the batch -- one command, cheap to replay. No grant on
    // the transforms; it never contends with their readers.
    auto propagation = g.add_node(
        [rec = w.transforms.recorder()](const Float_store& local_xf, const Float_store& bodies) mutable
        {
            std::vector<float> out(static_cast<std::size_t>(local_xf.size()));
            for (int i = 0, n = local_xf.size(); i < n; ++i)
                out[static_cast<std::size_t>(i)] = local_xf.get(i) + bodies.get(i);
            spin(1.0);
            rec.stage([batch = std::move(out)](Transforms& t) { t.apply(batch); });
        },
        w.local_xf, w.bodies);

    auto flip = g.add_node(ts::publish_body(w.transforms), w.transforms.state());
    flip.after(propagation);

    g.add_node(&tick_cloth, w.transforms.state(), w.cloth);
    g.add_node(&tick_culling, w.transforms.state(), w.renderables, w.visibility);
    g.add_node(&tick_particles, w.transforms.state(), w.particles);
    g.add_node(&tick_audio, w.transforms.state(), w.audio_out);
    g.add_node(&tick_render, w.transforms.state(), w.visibility, w.particles, w.draw_lists);
    g.add_node(&tick_ui, w.game_state, w.ui);
    // Debug overlay -- a bare generic lambda: `const auto&` classifies as a
    // read via the rvalue probe, same "const = read" rule as everywhere else.
    g.add_node([](const auto& game_state, const auto& xf)                         // 0.2
    {
        read_all(game_state);
        read_all(xf);
        spin(0.2);
    }, w.game_state, w.transforms.state());

    g.compile();
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

void run_game_frame_sample(int frames, float scale)
{
    double avg_ms = 0.0, serial_ms = 0.0;
    float transform0 = 0.0f;
    game_frame_stats(frames, scale, avg_ms, serial_ms, transform0);

    std::printf("\n[game_frame] %d frames, 1000 entities, scale %.2f\n", frames, scale);
    std::printf("  %.2f ms/frame  (serial budget %.2f ms -> %.2fx speedup)\n",
        avg_ms, serial_ms, serial_ms / avg_ms);
    std::printf("  peak %d concurrent nav queries (Guarded::async during AI), %d early-outed on cancel\n",
        nav_peak.load(), nav_early.load());
    std::printf("  streamed %d assets via then, %d batches via when_all\n",
        streamed.load(), batches.load());
}

} // namespace sample
