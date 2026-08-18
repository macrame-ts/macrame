#pragma once

// `run_benchmarks` (benchmarks.cpp) is the driver; the group entries below live one
// per TU and print their own group headers.
void run_benchmarks();

void run_scheduler_bench();        // scheduler_bench.cpp
void run_features_bench();         // features_bench.cpp
void run_coro_resume_bench();      // features_bench.cpp
void run_staged_commands_bench();  // staged_commands_bench.cpp
void run_access_control_bench();   // access_control_bench.cpp
void run_global_bottleneck_bench();   // global_bottleneck_bench.cpp
void run_subsystem_contention_bench();   // subsystem_contention_bench.cpp
void run_game_frame_bench();       // game_frame_bench.cpp
