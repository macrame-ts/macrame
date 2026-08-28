#include "benchmarks.h"
#include "bench_harness.h"

#include <cstdio>
#include <thread>

// The driver only. Each group lives in its own TU and prints its own headers:
//   scheduler_bench.cpp       - throughput / wake latency / contention / fork-join
//   features_bench.cpp        - pipe contention, the public-feature series, and the
//                               coroutine-resume decomposition grid
//   staged_commands_bench.cpp - the Deferred closure-tier cost series
//   production_contention_bench.cpp - the generalized production lock-contention case
//                               (docs/internals/lock-contention-research.md), pure mutex baseline
//   production_contention_lib_bench.cpp - the same workload architected on the library
//                               (staged touches, per-wave commit, apply-lane slow op)
//   game_frame_bench.cpp      - graph vs graph-free frame composition
// The call order below is the published output order - keep it stable so recorded
// baselines stay diffable.
void run_benchmarks()
{
    unsigned hw = std::thread::hardware_concurrency();
    std::printf("\nbenchmarks: %d reps x %lldms target, %u hw threads\n",
        reps, static_cast<long long>(target.count()), hw);

    run_scheduler_bench();
    run_features_bench();
    run_staged_commands_bench();
    run_production_contention_bench();
    run_production_contention_lib_bench();
    run_coro_resume_bench();
    run_game_frame_bench();
}
