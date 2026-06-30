#pragma once

namespace sample
{

// Result of driving the engine for some frames — for tests to assert against.
struct Frame_stats
{
    int frames;
    double avg_ms;        // measured wall time per frame
    double serial_ms;     // sum of system budgets at this scale (the baseline)
    float world_xf_value; // a deterministic frame output (propagation result)
};

// Build the world + frame graph and run `frames` frames at the given time scale.
// Returns when every frame has completed (a return == no deadlock). The world is
// created and destroyed inside, so all fire-and-forget async work drains too.
Frame_stats run_frames(int frames, float time_scale);

// Same, but prints a human-readable summary (showcase). 1.0 = real AAA @ 60 fps.
void run_sample(int frames = 20, float time_scale = 1.0f);

} // namespace sample
