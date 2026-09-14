// A fixed-rate graph beside a variable-rate frame graph. A physics world ticks at 60 Hz on
// its own clock (`ts::Periodic`), while the frame loop runs the frame graph as fast as it can.
// Neither loop knows the other's rate, and the frame loop has no accumulator and no catch-up
// path: how many ticks fall into a frame is the clock's business.
//
// The boundary between the two is two objects, the only state both graphs touch:
//   - `intents`, a `Deferred<Physics_world>`: gameplay stages impulses grant-free at any
//     moment, and the tick's step node commits them at the start of the tick. Whatever was
//     staged before that commit belongs to that tick; anything later waits for the next.
//   - `poses`, a `Versioned<Pose_snapshot, History::current_and_previous>`: the tick publishes
//     an extract of the world at its end, and render reads the last two published versions
//     together, interpolating by where the frame falls between their publish instants.
// The world itself has one accessor, the step node, so no frame code can reach it.
//
// The physics graph's nodes are `high` (`set_default_priority`), and gameplay's long loop and
// every `parallel_for` chunk boundary are yield points (`ts::yield`), so a tick that comes due
// while the workers are busy with frame work starts within one chunk of the frame's.
//
// Checked every run: ticks run one at a time in order, the interpolated pair is always two
// consecutive ticks, the interpolation fraction stays within [0, 1], and every staged intent is
// applied exactly once. Which tick picks up a given intent depends on wall time, as it would with
// a real input device, so the full run is not bit-reproducible; the physics graph alone, driven
// by a tick-indexed intent script, is (`fixed_rate_physics_hash`).

#include "ts/coroutine_support.h"
#include "ts/deferred.h"
#include "ts/guarded.h"
#include "ts/parallel_for.h"
#include "ts/static_task_graph.h"
#include "ts/task.h"
#include "ts/timer.h"
#include "ts/versioned.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace sample
{

namespace
{

using Clock = std::chrono::steady_clock;

constexpr int body_count = 256;
constexpr float tick_dt = 1.0f / 60.0f;
constexpr auto tick_period = std::chrono::microseconds(16'667);
// More ticks than this in one wake are dropped rather than run back to back: a clock that
// fell far behind slows simulated time down instead of stalling every frame behind a burst.
constexpr int max_ticks_per_wake = 2;

// Stand-in cost for work the sample does not model, kept on the CPU the way real work is.
void spin_for(std::chrono::microseconds duration)
{
    const auto until = Clock::now() + duration;
    while (Clock::now() < until)
    {
    }
}

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;
};

// The simulation. Sealed: the step node's write grant is its only accessor.
class Physics_world
{
public:
    Physics_world()
        : pos_(body_count)
        , vel_(body_count)
    {
        for (int i = 0; i < body_count; ++i)
            pos_[static_cast<std::size_t>(i)] = { static_cast<float>(i % 16), 10.0f + static_cast<float>(i / 16) };
    }

    void add_impulse(int body, Vec2 dv)
    {
        TS_CHECK_ACCESS();
        Vec2& v = vel_[static_cast<std::size_t>(body)];
        v.x += dv.x;
        v.y += dv.y;
        ++impulses_applied_;
    }

    void step(float dt)
    {
        TS_CHECK_ACCESS();
        for (std::size_t i = 0; i < pos_.size(); ++i)
        {
            vel_[i].y -= 9.8f * dt;
            pos_[i].x += vel_[i].x * dt;
            pos_[i].y += vel_[i].y * dt;
            if (pos_[i].y < 0.0f)
            {
                pos_[i].y = -pos_[i].y;
                vel_[i].y = -vel_[i].y * 0.8f;
            }
        }
        ++tick_;
    }

    const std::vector<Vec2>& positions() const { TS_CHECK_ACCESS(); return pos_; }
    std::uint64_t tick() const { TS_CHECK_ACCESS(); return tick_; }
    long long impulses_applied() const { TS_CHECK_ACCESS(); return impulses_applied_; }

    std::size_t hash() const
    {
        TS_CHECK_ACCESS();
        std::size_t h = 1469598103934665603ull;
        for (const Vec2& p : pos_)
        {
            h = (h ^ static_cast<std::size_t>(p.x * 1000.0f)) * 1099511628211ull;
            h = (h ^ static_cast<std::size_t>(p.y * 1000.0f)) * 1099511628211ull;
        }
        return h;
    }

private:
    std::vector<Vec2> pos_;
    std::vector<Vec2> vel_;
    std::uint64_t tick_ = 0;
    long long impulses_applied_ = 0;
};

// The extract a tick publishes: positions and the tick that produced them.
class Pose_snapshot
{
public:
    void assign(const std::vector<Vec2>& positions, std::uint64_t tick)
    {
        TS_CHECK_ACCESS();
        positions_ = positions;
        tick_ = tick;
    }

    Vec2 at(int body) const
    {
        TS_CHECK_ACCESS();
        return body < static_cast<int>(positions_.size()) ? positions_[static_cast<std::size_t>(body)] : Vec2{};
    }

    std::uint64_t tick() const { TS_CHECK_ACCESS(); return tick_; }

private:
    std::vector<Vec2> positions_;
    std::uint64_t tick_ = 0;
};

struct Input_state
{
    long long frame = 0;
};

struct Gameplay_state
{
    Vec2 focus;   // what render centres on: body 0, as the frame last saw it
};

struct Render_output
{
    float body0_y = 0.0f;   // interpolated between the last two ticks
    float focus_y = 0.0f;   // as gameplay last saw it
};

// The costs that stand in for real work, scaled together.
struct Costs
{
    std::chrono::microseconds physics;
    std::chrono::microseconds gameplay;
    std::chrono::microseconds render;
};

Costs costs_at(float scale)
{
    auto us = [scale](double value) { return std::chrono::microseconds(static_cast<long long>(value * scale)); };
    return { us(3000.0), us(4000.0), us(1500.0) };
}

// What the run observed, written by the nodes and read after both loops stop.
struct Run_stats
{
    std::atomic<long long> intents_staged{ 0 };
    std::atomic<long long> render_frames{ 0 };
    std::atomic<long long> pairs_not_consecutive{ 0 };
    std::atomic<long long> fraction_out_of_range{ 0 };
    long long ticks = 0;     // written by the driver only
    long long wakes = 0;
    long long dropped = 0;
    Clock::duration shortest_wake_interval = Clock::duration::max();
    Clock::duration longest_wake_interval = Clock::duration::zero();
};

// Both domains' state. Declared before the graphs that hold recorders into it, so the graphs
// are destroyed first.
struct Domains
{
    ts::Guarded<Physics_world> world{ ts::Named{ "world" } };
    ts::Deferred<Physics_world> intents{ world };
    ts::Versioned<Pose_snapshot, ts::History::current_and_previous> poses{ ts::Named{ "poses" } };

    ts::Guarded<Input_state> input{ ts::Named{ "input" } };
    ts::Guarded<Gameplay_state> gameplay{ ts::Named{ "gameplay" } };
    ts::Guarded<Render_output> render{ ts::Named{ "render" } };
};

// One tick: commit the intents staged since the last tick, step, publish the extract.
ts::Static_task_graph build_physics_graph(Domains& domains, Costs costs)
{
    ts::Static_task_graph graph;
    auto step = graph.add_node("physics_step",
        [&domains, costs, out = domains.poses.recorder()](Physics_world& world) mutable
        {
            (void)domains.intents.commit();   // inline: this node holds the write grant
            world.step(tick_dt);
            // The solver's cost, fanned out; the chunks inherit the node's `high` priority.
            ts::parallel_for(4, [costs](int) { spin_for(costs.physics / 4); });
            out.stage([positions = world.positions(), tick = world.tick()](Pose_snapshot& snapshot)
            {
                snapshot.assign(positions, tick);
            });
        },
        domains.world);
    graph.add_node("physics_publish", ts::publish_fn(domains.poses), domains.poses.state()).after(step);
    graph.set_default_priority(ts::Priority::high);
    graph.compile();
    return graph;
}

// The frame: input, gameplay (stages intents, reads the last snapshot), render (interpolates).
ts::Static_task_graph build_frame_graph(Domains& domains, Costs costs, Run_stats& stats)
{
    ts::Static_task_graph graph;
    graph.add_node("input", [](Input_state& input) { ++input.frame; }, domains.input);

    graph.add_node("gameplay",
        [costs, &stats, rec = domains.intents.recorder()](const Input_state& input, const Pose_snapshot& poses,
            Gameplay_state& gameplay) mutable
        {
            gameplay.focus = poses.at(0);
            if (input.frame % 20 == 0)
            {
                const int body = static_cast<int>(input.frame / 20 % body_count);
                rec.stage([body](Physics_world& world) { world.add_impulse(body, { 0.0f, 6.0f }); });
                stats.intents_staged.fetch_add(1, std::memory_order_relaxed);
            }
            // A long body with yield points: a tick that comes due mid-loop runs here, between
            // two slices, instead of waiting for the whole body.
            for (int slice = 0; slice < 8; ++slice)
            {
                spin_for(costs.gameplay / 8);
                ts::yield();
            }
        },
        domains.input, domains.poses.state(), domains.gameplay);

    graph.add_node("render",
        [&domains, costs, &stats](const Gameplay_state& gameplay, const Pose_snapshot&, Render_output& render)
        {
            // Lent: this node declared a read on the front, so the pair takes no turn of its own.
            auto [previous, current, stamps] = domains.poses.read_last_versions();
            const double alpha = stamps.fraction_at(Clock::now());
            if (current.tick() != previous.tick() + 1)
                stats.pairs_not_consecutive.fetch_add(1, std::memory_order_relaxed);
            if (alpha < 0.0 || alpha > 1.0)
                stats.fraction_out_of_range.fetch_add(1, std::memory_order_relaxed);
            const float y0 = previous.at(0).y;
            const float y1 = current.at(0).y;
            render.body0_y = y0 + static_cast<float>(alpha) * (y1 - y0);
            render.focus_y = gameplay.focus.y;
            spin_for(costs.render);
            stats.render_frames.fetch_add(1, std::memory_order_relaxed);
        },
        domains.gameplay, domains.poses.state(), domains.render);

    graph.compile();
    return graph;
}

// The fixed-rate driver: wait for the clock, run the ticks it reports, repeat until stopped.
// The overload policy lives here and nowhere else.
ts::Task<void> run_ticks(ts::Static_task_graph& physics, ts::Periodic& clock, Run_stats& stats)
{
    Clock::time_point last_wake{};
    for (;;)
    {
        const int due = co_await clock.next();
        if (due == 0)
            co_return;   // stopped
        const Clock::time_point now = Clock::now();
        if (stats.wakes > 0)
        {
            stats.shortest_wake_interval = std::min(stats.shortest_wake_interval, now - last_wake);
            stats.longest_wake_interval = std::max(stats.longest_wake_interval, now - last_wake);
        }
        last_wake = now;
        ++stats.wakes;
        const int run = std::min(due, max_ticks_per_wake);
        stats.dropped += due - run;
        for (int i = 0; i < run; ++i)
            co_await physics.execute();
        stats.ticks += run;
    }
}

struct Outcome
{
    bool ok = false;
    double frame_ms = 0.0;
    double ticks_per_second = 0.0;
};

Outcome run_domains(int frames, float scale, bool print)
{
    const Costs costs = costs_at(scale);
    Run_stats stats;
    Domains domains;
    ts::Static_task_graph physics = build_physics_graph(domains, costs);
    ts::Static_task_graph frame = build_frame_graph(domains, costs, stats);

    // Two ticks before the loops start, so the pair render reads holds two real versions.
    physics.execute().sync();
    physics.execute().sync();

    ts::Cancellation_source stop;
    ts::Periodic clock{ tick_period, { .token = stop.token(), .priority = ts::Priority::high } };
    ts::Task<void> driver = run_ticks(physics, clock, stats);

    const Clock::time_point t0 = Clock::now();
    for (int f = 0; f < frames; ++f)
        frame.execute().sync();
    const double elapsed_s = std::chrono::duration<double>(Clock::now() - t0).count();

    stop.request_cancel();
    driver.sync();
    physics.execute().sync();   // one more tick commits whatever the last frames staged

    const long long ticks_total = stats.ticks + 3;   // the two primers and the final tick
    const auto [world_ticks, applied] = domains.world.access([](const Physics_world& world)
    {
        return std::pair{ static_cast<long long>(world.tick()), world.impulses_applied() };
    }).sync();

    Outcome outcome;
    outcome.frame_ms = 1000.0 * elapsed_s / frames;
    outcome.ticks_per_second = elapsed_s > 0.0 ? static_cast<double>(stats.ticks) / elapsed_s : 0.0;
    outcome.ok = world_ticks == ticks_total
        && applied == stats.intents_staged.load()
        && stats.pairs_not_consecutive.load() == 0
        && stats.fraction_out_of_range.load() == 0
        && stats.render_frames.load() == frames;

    if (print)
    {
        auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
        std::printf("\n[fixed_rate] %d frames at %.2f ms/frame; %lld ticks in %.2f s = %.1f ticks/s (period %.2f ms)\n",
            frames, outcome.frame_ms, stats.ticks, elapsed_s, outcome.ticks_per_second,
            std::chrono::duration<double, std::milli>(tick_period).count());
        if (stats.wakes > 1)
        {
            std::printf("  wake intervals %.2f .. %.2f ms, %lld ticks dropped by the overload policy\n",
                ms(stats.shortest_wake_interval), ms(stats.longest_wake_interval), stats.dropped);
        }
        std::printf("  %lld intents staged, %lld applied; pairs consecutive: %s; fraction in [0, 1]: %s -> %s\n",
            stats.intents_staged.load(), applied, stats.pairs_not_consecutive.load() == 0 ? "yes" : "no",
            stats.fraction_out_of_range.load() == 0 ? "yes" : "no", outcome.ok ? "ok" : "FAILED");
    }
    return outcome;
}

} // namespace

// --- entry points -----------------------------------------------------------------

// Runs both loops for `frames` frames and prints what the fixed-rate side did.
void run_fixed_rate_sample(int frames)
{
    (void)run_domains(frames, 1.0f, true);
}

// The structural checks of a run, for the integration test (true = every check held).
bool fixed_rate_self_check(int frames, float scale)
{
    return run_domains(frames, scale, false).ok;
}

// The physics graph alone for `ticks` ticks, with intents staged on a tick-indexed script from
// the calling thread: the world's hash is a function of the script only, whatever the worker
// count - the determinism a fixed-rate graph keeps when its input cut sequence is fixed.
std::size_t fixed_rate_physics_hash(int ticks)
{
    Run_stats stats;
    Domains domains;
    ts::Static_task_graph physics = build_physics_graph(domains, costs_at(0.02f));
    {
        auto rec = domains.intents.recorder();
        for (int tick = 0; tick < ticks; ++tick)
        {
            if (tick % 7 == 0)
            {
                const int body = tick % body_count;
                rec.stage([body](Physics_world& world) { world.add_impulse(body, { 1.0f, 4.0f }); });
            }
            physics.execute().sync();
        }
    }
    return domains.world.access([](const Physics_world& world) { return world.hash(); }).sync();
}

// Headless run at a fast scale, for the sanitizer driver: both graphs, the clock, the yield
// points and the `Versioned` pair read under concurrency.
void stress_fixed_rate(int frames)
{
    (void)run_domains(frames, 0.05f, false);
}

} // namespace sample
