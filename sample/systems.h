#pragma once

#include "world.h"

namespace sample
{

// Scales every system's mock cost. 1.0 = real AAA @ 60 fps budgets; tests use a
// small scale so many frames run fast.
extern float time_scale;

// Per-frame system ticks. The parameter const-ness is the access declaration the
// frame graph reads: T& = read_write, const T& = read_only.

void tick_input(Float_store& velocities);                                              // 0.2 ms
void tick_animation(const Float_store& skeletons, Float_store& local_xf);              // 3.0 ms
void tick_physics(const Float_store& velocities, Float_store& bodies);                 // 3.0 ms
void tick_propagation(const Float_store& local_xf, const Float_store& bodies,
                      Float_store& world_xf);                                          // 1.0 ms
void tick_render(const Float_store& world_xf);                                         // 2.5 ms

// Sum of the above budgets (ms) at scale 1.0 — the serial baseline to beat.
double serial_budget_ms();

} // namespace sample
