#pragma once

#include "world.h"

namespace sample
{

// Scales every system's mock cost. 1.0 = real AAA @ 60 fps budgets; tests use a
// small scale so many frames run fast.
extern float time_scale;

// Per-frame system ticks. The parameter const-ness is the access declaration the
// frame graph reads: `T&` = `read_write`, `const T&` = `read_only`. Budgets
// (ms @ 1.0) in comments; serial sum is `serial_budget_ms()`.

void tick_input(Float_store& input);                                                   // 0.1
void tick_networking(const Float_store& input, Float_store& net);                      // 0.5
// Streaming loads assets from a read-only source via `Guarded::async`, then
// processes each with `then` and fires a batch finalize with `when_all` -- so it
// takes the source wrapper, not an unwrapped store.
void tick_streaming(ts::Guarded<Float_store>& asset_source,
                    const Float_store& input, Float_store& assets);                    // 0.5
void tick_gameplay(const Float_store& world_xf_prev, const Float_store& input,
                   const Float_store& net, Float_store& game_state);                   // 2.0
void tick_navigation(const Float_store& nav, const Float_store& world_xf_prev,
                     Float_store& paths);                                              // 1.0
// AI issues per-agent path queries against the read-only `nav` service via the
// `Guarded::async` path, so it takes the wrapper, not an unwrapped store.
void tick_ai(ts::Guarded<Float_store>& nav,
             const Float_store& world_xf_prev, const Float_store& paths,
             const Float_store& game_state, Float_store& intents);                     // 1.5
void tick_animation(const Float_store& skeletons, const Float_store& intents,
                    Float_store& local_xf);                                            // 3.0
void tick_physics(const Float_store& velocities, const Float_store& game_state,
                  Float_store& bodies);                                                // 3.0
void tick_propagation(const Float_store& local_xf, const Float_store& bodies,
                      Float_store& world_xf);                                          // 1.0
void tick_cloth(const Float_store& world_xf, Float_store& cloth);                      // 1.0
void tick_culling(const Float_store& world_xf, const Float_store& renderables,
                  Float_store& visibility);                                            // 1.5
void tick_particles(const Float_store& world_xf, Float_store& particles);              // 1.5
void tick_audio(const Float_store& world_xf, Float_store& audio_out);                  // 0.5
void tick_render(const Float_store& world_xf, const Float_store& visibility,
                 const Float_store& particles, Float_store& draw_lists);               // 2.5
void tick_ui(const Float_store& game_state, Float_store& ui);                          // 0.5
void tick_debug(const Float_store& game_state, const Float_store& world_xf);           // 0.2
void tick_swap(const Float_store& world_xf, Float_store& world_xf_prev);               // 0.1

// Sum of all budgets (ms) at scale 1.0 — the serial baseline to beat.
double serial_budget_ms();

// Reset per-run instrumentation.
void reset_stats();
int observed_nav_concurrency();   // peak concurrent `nav` queries (`Guarded::async`)
int nav_early_outs();             // nav queries that early-outed on the cancel token mid-body
int assets_streamed();            // asset loads finished via a `then` continuation
int batches_streamed();           // load batches finished via a `when_all` continuation

} // namespace sample
