#pragma once

#include <cstddef>

namespace sample
{

// Results of driving the physics demo for some frames -- for assertions and the
// determinism self-check.
struct Physics_stats
{
    int frames = 0;
    int bodies = 0;              // final body count in the world
    int spawned = 0;             // bodies created via staged spawns
    int vision_hits = 0;         // raycast hits accumulated by the AI node
    float player_y = 0.0f;       // final published pose of the player body
    std::size_t pose_hash = 0;   // bitwise hash of the final published snapshot
};

// The `Deferred`/`Versioned`/`Guarded` decomposition from the command-buffer
// design study: a sealed `Guarded<Physics_world>` machine, a `Deferred` staging
// its external inputs (impulses, spawns), and a `Versioned<Pose_snapshot>`
// publishing its outputs. Gameplay reads last frame's poses all frame; the flip
// is the only write conflict on the snapshot.
Physics_stats run_physics_frames(int frames);

// Runs the demo (twice -- the second run proves run-to-run determinism) and
// prints a summary.
void run_physics_sample(int frames = 60);

} // namespace sample
