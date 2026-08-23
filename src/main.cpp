#include "tests.h"
#include "benchmarks.h"
#include "mem_profile.h"
#include "harness.h"
#include "ts/fatal.h"
#include "ts/scheduler.h"
#include "ts/version.h"

// Single-file samples (by design - no headers): the mock game-engine frame
// (sample/game_frame.cpp), the physics machine/extract decomposition
// (sample/physics.cpp), the blackboard pattern recipe of
// docs/command-buffer-design.md §7.4 (sample/blackboard.cpp), and the
// delegate/event recipe (sample/events.cpp).
namespace sample
{
void run_game_frame_sample(int frames = 20, float time_scale = 1.0f);
void dump_game_frame_dot(const char* path);
void trace_game_frame(int frames, const char* DOT_path, const char* SVG_path);
void run_physics_sample(int frames = 60);
void run_blackboard_sample();
void run_events_sample();
void run_coloring_sample();
void run_scope_access_sample();
}

#include <cstdio>
#include <cstdlib>
#include <cstring>

// The TSan stress stages' entry, renamed from `main` under `TS_TSAN_NO_MAIN` (tsan/tsan_main.cpp).
namespace ts::tsan { int run_all(); }

namespace
{

// Non-test entries (bench / stress / samples) have no harness to catch an ensure
// failure - fail their exit code instead, so CI cannot pass a run that tripped one.
int exit_checking_ensure_failures(int code)
{
#if TS_SAFETY_CHECKS
    if (ts::ensure_failure_count() != 0)
    {
        std::fprintf(stderr, "\n%lld ENSURE failure(s) fired - failing the run\n",
            ts::ensure_failure_count());
        return code == 0 ? 1 : code;
    }
#endif
    return code;
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: last line is visible if a test crashes
    ts::install_crash_handler();                 // raw AV / stack overflow self-reports (stack + minidump)

    if (argc >= 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0))
    {
        std::printf("macrame %s\n", TS_VERSION_STRING);
        return 0;
    }

    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0))
    {
        std::printf(
            "macrame %s - test/benchmark/sample driver\n\n"
            "usage: macrame_playground [MODE]\n\n"
            "  (no args)      run the test suite, all samples, and the benchmarks\n"
            "  --tests        run the test suite only; exit code = failure count\n"
            "    --repeat N   ... N times in one process, stopping at the first failing run\n"
            "                 (a race at 1-in-N is invisible to a single run)\n"
            "  --workers N    pin the scheduler width (any mode); the default is the hardware\n"
            "                 width, which on a wide machine hides the contended interleavings\n"
            "                 a 2-core CI runner produces every time\n"
            "  --bench        run the benchmarks only\n"
            "  --stress       run the sample many frames at a fast scale (for sanitizers)\n"
            "  --dot [path]   write the game_frame graph structure as Graphviz DOT\n"
            "                 (default sample_game_frame.dot; render with show_graph.bat)\n"
            "  --trace [n]    run the game_frame sample n frames (default 200) with an\n"
            "                 aggregating trace, for the baseline and optimised variants\n"
            "                 on 8 workers; writes sample_game_frame_avg_baseline.svg and\n"
            "                 sample_game_frame_avg_optimised.svg plus sample_game_frame.dot\n"
            "  --memprofile   run the allocation profiler (needs a TS_MEM_PROFILE build)\n"
            "  --version, -v  print the version\n"
            "  --help, -h     show this message\n",
            TS_VERSION_STRING);
        return 0;
    }

    // Every mode below runs scheduled work, so bring the process-wide scheduler up here (after
    // the no-op --version/--help paths). Torn down at program exit by the holder's safety net.
    //
    // `--workers N` (anywhere on the line) pins the width. The suite otherwise runs at the
    // machine's hardware width, and a race that needs a particular interleaving shows at one
    // width and not another: the CI runner has 2 cores, a development machine may have 20,
    // and "passes locally" and "passes on CI" are then two different measurements. A narrow
    // scheduler produces the contended interleavings deterministically, rather than when the
    // OS happens to starve the process.
    // The TSan stress stages (tsan/tsan_main.cpp), compiled into this driver under
    // `TS_TSAN_NO_MAIN` so one instrumented binary serves both the suite and the stress run.
    // `ts::tsan::run_all` brings up its own scheduler, so it dispatches here, ahead of the
    // `create_scheduler` below - a second one is a fatal.
    if (argc >= 2 && std::strcmp(argv[1], "--tsan-stress") == 0)
        return ts::tsan::run_all();

    ts::Scheduler_config config;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--workers") == 0)
            config.num_workers = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
    }
    ts::create_scheduler(config);

    // Death-test child: run one fatal scenario (it is expected to abort).
    if (argc >= 3 && std::strcmp(argv[1], "--death") == 0)
    {
        ts::test::prepare_death_child();
        run_death_scenario(argv[2]);
        return 0;   // reaching here means the scenario failed to abort
    }

    // Structure dump: compile the sample's frame graph, write the DOT file, exit.
    if (argc >= 2 && std::strcmp(argv[1], "--dot") == 0)
    {
        const char* path = argc >= 3 ? argv[2] : "sample_game_frame.dot";
        sample::dump_game_frame_dot(path);
        std::printf("wrote %s\n", path);
        return 0;
    }

    // Aggregated trace: run the frame sample with a Graph_trace attached; writes the
    // average-run SVG plus the structure DOT.
    if (argc >= 2 && std::strcmp(argv[1], "--trace") == 0)
    {
        int frames = argc >= 3 ? std::atoi(argv[2]) : 200;
        if (frames <= 0)
            frames = 200;
        sample::trace_game_frame(frames, "sample_game_frame.dot", "sample_game_frame_avg.svg");
        return 0;
    }

    // Stress entry: just the sample, many frames, fast scale (for sanitizers).
    if (argc >= 2 && std::strcmp(argv[1], "--stress") == 0)
    {
        sample::run_game_frame_sample(2000, 0.2f);
        return exit_checking_ensure_failures(0);
    }

    // Isolation entries (for narrowing down a flaky crash).
    if (argc >= 2 && std::strcmp(argv[1], "--bench") == 0)
    {
        run_benchmarks();
        return exit_checking_ensure_failures(0);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--memprofile") == 0)
    {
        run_mem_profile();
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--tests") == 0)
    {
        // `--tests --repeat N`: the suite N times in one process, stopping at the first
        // failing run and naming it. A race at 1-in-N survives a single run with probability
        // (N-1)/N, so "passes" means little for concurrent code until it means "passes N
        // times" - and N is the dial. The run number is printed up front because the one
        // piece of information a flake hunt needs is which run to read.
        int repeat = 1;
        if (argc >= 4 && std::strcmp(argv[2], "--repeat") == 0)
            repeat = std::atoi(argv[3]);
        if (repeat < 1)
            repeat = 1;
        for (int i = 1; i <= repeat; ++i)
        {
            if (repeat > 1)
                std::printf("\n===== run %d / %d =====\n", i, repeat);
            ts::test::reset();
            run_all_tests();
            if (int code = ts::test::summary(); code != 0)
            {
                if (repeat > 1)
                    std::fprintf(stderr, "\nrun %d / %d failed - stopping\n", i, repeat);
                return code;
            }
        }
        if (repeat > 1)
            std::printf("\n%d / %d runs clean\n", repeat, repeat);
        return 0;
    }

    run_all_tests();
    sample::run_game_frame_sample();
    sample::run_physics_sample();
    sample::run_blackboard_sample();
    sample::run_events_sample();
    sample::run_coloring_sample();
    sample::run_scope_access_sample();
    run_benchmarks();

    return ts::test::summary();
}
