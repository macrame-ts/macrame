#include "physics.h"

#include "access.h"
#include "deferred.h"
#include "guarded.h"
#include "parallel_for.h"
#include "static_task_graph.h"
#include "versioned.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace sample
{

namespace
{

// --- math ----------------------------------------------------------------------

struct Vec3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }

using Body_id = int;

struct Pose
{
    Body_id id = 0;
    Vec3 pos;
    Vec3 vel;
};

// Grant-free id reservation: gameplay gets a body's identity at STAGE time even
// though the body exists only once the sim commits the staged creation -- the
// forward-reference pattern (no reserved-handle support needed in `Deferred`).
struct Body_id_allocator
{
    std::atomic<Body_id> next{ 1 };
    Body_id reserve() { return next.fetch_add(1, std::memory_order_relaxed); }
};

// --- the machine ------------------------------------------------------------------

// The heavy internal state: single instance, never replicated, never copied.
// Exactly two graph accessors -- `vision` (read) and `sim` (write); everything
// else stages into the machine's `Deferred` or reads the published snapshot.
class Physics_world
{
public:
    void create_body(Body_id id, Vec3 pos, Vec3 vel)
    {
        TS_CHECK_ACCESS();
        bodies_.push_back({ id, pos, vel });
    }

    void add_impulse(Body_id id, Vec3 dv)
    {
        TS_CHECK_ACCESS();
        for (Body& b : bodies_)
            if (b.id == id)
            {
                b.vel = b.vel + dv;
                return;
            }
    }

    void step(float dt)
    {
        TS_CHECK_ACCESS();
        constexpr Vec3 gravity{ 0.0f, -9.8f, 0.0f };
        for (Body& b : bodies_)
        {
            b.vel = b.vel + gravity * dt;
            b.pos = b.pos + b.vel * dt;
            if (b.pos.y < 0.0f)   // ground: clamp + damped bounce
            {
                b.pos.y = 0.0f;
                b.vel.y = -b.vel.y * 0.5f;
            }
        }
    }

    // The frame's output delta: every body's pose (toy world -- all bodies are
    // active). Pure data; cheap and deterministic to replay twice.
    std::vector<Pose> extract_poses() const
    {
        TS_CHECK_ACCESS();
        std::vector<Pose> out;
        out.reserve(bodies_.size());
        for (const Body& b : bodies_)
            out.push_back({ b.id, b.pos, b.vel });
        return out;
    }

    // Scene query -- needs the machine (readers of the snapshot can't answer
    // spatial questions). Toy: does a downward ray from `from` pass within
    // `radius` (in x/z) of any body below it?
    bool raycast_down(Vec3 from, float radius) const
    {
        TS_CHECK_ACCESS();
        for (const Body& b : bodies_)
        {
            float dx = b.pos.x - from.x;
            float dz = b.pos.z - from.z;
            if (b.pos.y <= from.y && dx * dx + dz * dz <= radius * radius)
                return true;
        }
        return false;
    }

    int body_count() const
    {
        TS_CHECK_ACCESS();
        return static_cast<int>(bodies_.size());
    }

private:
    struct Body
    {
        Body_id id;
        Vec3 pos;
        Vec3 vel;
    };

    std::vector<Body> bodies_;
};

// --- the game-facing output ---------------------------------------------------------

// Small, flat, POD-valued -- the extract, not the machine. `apply` is idempotent
// stores keyed by id, so the replay resync is trivially deterministic and bodies
// inactive in a frame keep their (still correct) old values.
class Pose_snapshot
{
public:
    void apply(const std::vector<Pose>& batch)
    {
        TS_CHECK_ACCESS();
        for (const Pose& p : batch)
            poses_[p.id] = p;
    }

    Pose of(Body_id id) const
    {
        TS_CHECK_ACCESS();
        auto it = poses_.find(id);
        return it != poses_.end() ? it->second : Pose{};
    }

    // All published poses, in id order (deterministic iteration).
    std::vector<Pose> all() const
    {
        TS_CHECK_ACCESS();
        std::vector<Pose> out;
        out.reserve(poses_.size());
        for (const auto& [id, pose] : poses_)
            out.push_back(pose);
        return out;
    }

    bool has(Body_id id) const
    {
        TS_CHECK_ACCESS();
        return poses_.contains(id);
    }

    std::size_t size() const
    {
        TS_CHECK_ACCESS();
        return poses_.size();
    }

    // Bitwise hash (FNV-1a over the pose bytes, deterministic map order) -- the
    // divergence-check hook and the sample's determinism fingerprint.
    std::size_t hash() const
    {
        TS_CHECK_ACCESS();
        std::size_t h = 14695981039346656037ull;
        auto mix = [&h](const void* data, std::size_t n)
        {
            const unsigned char* p = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < n; ++i)
            {
                h ^= p[i];
                h *= 1099511628211ull;
            }
        };
        for (const auto& [id, pose] : poses_)
        {
            mix(&id, sizeof id);
            mix(&pose.pos, sizeof pose.pos);
            mix(&pose.vel, sizeof pose.vel);
        }
        return h;
    }

private:
    std::map<Body_id, Pose> poses_;   // ordered: deterministic hash iteration
};

// --- gameplay systems -----------------------------------------------------------------

class Player
{
public:
    void set_body(Body_id id) { TS_CHECK_ACCESS(); body_ = id; }
    Body_id body() const { TS_CHECK_ACCESS(); return body_; }

    // Hover controller off LAST frame's pose: thrust up when low. Deterministic.
    Vec3 thrust_from(const Pose& pose) const
    {
        TS_CHECK_ACCESS();
        return pose.pos.y < 3.0f ? Vec3{ 0.0f, 0.4f, 0.0f } : Vec3{};
    }

private:
    Body_id body_ = 0;
};

class Spawn_queue
{
public:
    void request(Vec3 pos) { TS_CHECK_ACCESS(); pending_.push_back(pos); }

    // Drain up to `n` requests this frame.
    std::vector<Vec3> take(int n)
    {
        TS_CHECK_ACCESS();
        std::vector<Vec3> out;
        while (!pending_.empty() && static_cast<int>(out.size()) < n)
        {
            out.push_back(pending_.back());
            pending_.pop_back();
        }
        return out;
    }

    void bound(Body_id id) { TS_CHECK_ACCESS(); spawned_.push_back(id); }
    int spawned_count() const { TS_CHECK_ACCESS(); return static_cast<int>(spawned_.size()); }

private:
    std::vector<Vec3> pending_;
    std::vector<Body_id> spawned_;
};

class Ai_system
{
public:
    void add_hit() { TS_CHECK_ACCESS(); ++hits_; }
    int hits() const { TS_CHECK_ACCESS(); return hits_; }

private:
    int hits_ = 0;
};

class Camera_state
{
public:
    void follow(Vec3 pos) { TS_CHECK_ACCESS(); pos_ = pos; }
    Vec3 pos() const { TS_CHECK_ACCESS(); return pos_; }

private:
    Vec3 pos_;
};

} // namespace

// --- the frame -----------------------------------------------------------------------

Physics_stats run_physics_frames(int frames)
{
    constexpr float dt = 1.0f / 60.0f;

    // Destruction order matters: graph first (it holds recorders), then the
    // Deferred/Versioned (their dtors verify nothing staged was lost), then the
    // world. All journals are empty at frame end by construction: gameplay's
    // stages are committed by `sim` the same frame, sim's extract is published by
    // `flip` the same frame.
    ts::Guarded<Physics_world> world;
    ts::Deferred<Physics_world> world_in{ world };      // the machine's staged inputs
    ts::Versioned<Pose_snapshot> poses;                 // the machine's published outputs
    Body_id_allocator ids;

    ts::Guarded<Player> player;
    ts::Guarded<Spawn_queue> spawns;
    ts::Guarded<Ai_system> ai;
    ts::Guarded<Camera_state> camera;

    poses.set_divergence_check([](const Pose_snapshot& s) { return s.hash(); });

    // Seed: the player's body (id reserved grant-free, creation staged), plus a
    // batch of spawn requests the spawner drains over the first frames.
    const Body_id player_body = ids.reserve();
    {
        auto boot = world_in.recorder();
        boot.stage([player_body](Physics_world& w)
        {
            w.create_body(player_body, { 0.0f, 5.0f, 0.0f }, {});
        });
        player.async([player_body](Player& p) { p.set_body(player_body); }).sync();
        spawns.async([](Spawn_queue& q)
        {
            for (int i = 0; i < 8; ++i)
                q.request({ static_cast<float>(i) - 4.0f, 6.0f, 1.0f });
        }).sync();
    }

    ts::Static_task_graph g;

    // Gameplay: reads LAST frame's poses (stable all frame, overlaps everything),
    // stages this frame's inputs. No grant on `world` anywhere in this node.
    // The drag loop stages in PARALLEL through a per-worker recorder: placement
    // order is nondeterministic, but each body gets exactly one drag command and
    // distinct bodies commute at the world level -- and the player's thrust
    // (recorder created BEFORE the parallel slots) always applies before its
    // drag -- so the frame stays bit-deterministic across runs.
    auto gameplay = g.add_node(
        [rec = world_in.recorder(), drag = world_in.parallel_recorder()](const Pose_snapshot& p, const Player& pl) mutable
        {
            Vec3 f = pl.thrust_from(p.of(pl.body()));
            rec.stage([id = pl.body(), f](Physics_world& w) { w.add_impulse(id, f); });

            std::vector<Pose> bodies = p.all();
            ts::parallel_for(static_cast<int>(bodies.size()), [&drag, &bodies](int i)
            {
                const Pose& b = bodies[i];
                drag.stage([id = b.id, dv = b.vel * -0.05f](Physics_world& w)
                {
                    w.add_impulse(id, dv);
                });
            });
        },
        poses.state(), player);

    // Spawner: reserve ids now (forward reference), stage the creations.
    auto spawner = g.add_node(
        [rec = world_in.recorder(), &ids](Spawn_queue& q) mutable
        {
            for (Vec3 pos : q.take(2))
            {
                Body_id id = ids.reserve();
                rec.stage([id, pos](Physics_world& w) { w.create_body(id, pos, {}); });
                q.bound(id);
            }
        },
        spawns);

    // Scene query: the one gameplay read of the machine (broadphase question the
    // snapshot can't answer). Declared before `sim`, so conflict derivation runs
    // it against last frame's world, parallel with gameplay/spawner.
    g.add_node(
        [](const Physics_world& w, Ai_system& a)
        {
            if (w.raycast_down({ 0.0f, 100.0f, 1.0f }, 2.5f))
                a.add_hit();
        },
        world, ai);

    // The sim: exclusive write on the machine, heaviest node in the frame.
    // Boundary commit (under the grant this node already holds) -> step ->
    // batch-extract into ONE staged command (cheap to replay twice).
    auto sim = g.add_node(
        [&world_in, out = poses.recorder()](Physics_world& w) mutable
        {
            world_in.commit(w);
            w.step(dt);
            out.stage([batch = w.extract_poses()](Pose_snapshot& s) { s.apply(batch); });
        },
        world);
    sim.after(gameplay).after(spawner);   // this frame's inputs make this frame's step

    // The flip: the only write conflict on `poses`. `gameplay` (declared before
    // it) derives an edge in front of it; `camera` opts into freshness after it.
    auto flip = g.add_node(ts::publish_body(poses), poses.state());
    flip.after(sim);

    auto cam = g.add_node(
        [](const Pose_snapshot& p, const Player& pl, Camera_state& c)
        {
            c.follow(p.of(pl.body()).pos);
        },
        poses.state(), player, camera);
    cam.after(flip);

    g.compile();

    for (int f = 0; f < frames; ++f)
        g.execute().sync();

    Physics_stats stats;
    stats.frames = frames;
    stats.bodies = world.access([](const Physics_world& w) { return w.body_count(); }).sync();
    stats.spawned = spawns.access([](const Spawn_queue& q) { return q.spawned_count(); }).sync();
    stats.vision_hits = ai.access([](const Ai_system& a) { return a.hits(); }).sync();
    stats.player_y = poses.read([player_body](const Pose_snapshot& s) { return s.of(player_body).pos.y; }).sync();
    stats.pose_hash = poses.read([](const Pose_snapshot& s) { return s.hash(); }).sync();
    return stats;
}

void run_physics_sample(int frames)
{
    Physics_stats a = run_physics_frames(frames);
    Physics_stats b = run_physics_frames(frames);

    // Staged writes + keyed apply order + replay resync make the whole frame
    // deterministic regardless of thread timing: two runs must be bit-identical.
    const bool deterministic = a.pose_hash == b.pose_hash
        && std::memcmp(&a.player_y, &b.player_y, sizeof a.player_y) == 0
        && a.bodies == b.bodies;

    std::printf("physics sample: %d frames, %d bodies (%d spawned), %d vision hits, player y %.3f, %s\n",
        a.frames, a.bodies, a.spawned, a.vision_hits, a.player_y,
        deterministic ? "deterministic across runs" : "NON-DETERMINISTIC (bug)");
}

} // namespace sample
