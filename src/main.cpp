#include "tests.h"
#include "benchmarks.h"
#include "mem_profile.h"
#include "harness.h"
#include "ts/fatal.h"
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
        std::printf("task_system %s\n", TS_VERSION_STRING);
        return 0;
    }

    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0))
    {
        std::printf(
            "task_system %s - test/benchmark/sample driver\n\n"
            "usage: task_system [MODE]\n\n"
            "  (no args)      run the test suite, all samples, and the benchmarks\n"
            "  --tests        run the test suite only; exit code = failure count\n"
            "  --bench        run the benchmarks only\n"
            "  --stress       run the sample many frames at a fast scale (for sanitizers)\n"
            "  --dot [path]   write the game_frame graph structure as Graphviz DOT\n"
            "                 (default sample_game_frame.dot; render with show_graph.bat)\n"
            "  --trace [n]    run the game_frame sample n frames (default 200) with an\n"
            "                 aggregating trace, on the default scheduler and on a\n"
            "                 4-worker one; writes sample_game_frame_avg.svg and\n"
            "                 sample_game_frame_avg_4workers.svg plus sample_game_frame.dot\n"
            "  --memprofile   run the allocation profiler (needs a TS_MEM_PROFILE build)\n"
            "  --version, -v  print the version\n"
            "  --help, -h     show this message\n",
            TS_VERSION_STRING);
        return 0;
    }

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
        run_all_tests();
        return ts::test::summary();
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
