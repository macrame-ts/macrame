#include "tests.h"
#include "benchmarks.h"
#include "mem_profile.h"
#include "harness.h"
#include "engine.h"
#include "physics.h"
#include "ts/version.h"

// Single-file sample (sample/blackboard.cpp, by design -- no header): the
// blackboard pattern recipe of docs/command-buffer-design.md §7.4.
namespace sample { void run_blackboard_sample(); }

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: last line is visible if a test crashes

    if (argc >= 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0))
    {
        std::printf("task_system %s\n", TS_VERSION_STRING);
        return 0;
    }

    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0))
    {
        std::printf(
            "task_system %s -- test/benchmark/sample driver\n\n"
            "usage: task_system [MODE]\n\n"
            "  (no args)      run the test suite, all samples, and the benchmarks\n"
            "  --tests        run the test suite only; exit code = failure count\n"
            "  --bench        run the benchmarks only\n"
            "  --stress       run the sample many frames at a fast scale (for sanitizers)\n"
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

    // Stress entry: just the sample, many frames, fast scale (for sanitizers).
    if (argc >= 2 && std::strcmp(argv[1], "--stress") == 0)
    {
        sample::run_sample(2000, 0.2f);
        return 0;
    }

    // Isolation entries (for narrowing down a flaky crash).
    if (argc >= 2 && std::strcmp(argv[1], "--bench") == 0)
    {
        run_benchmarks();
        return 0;
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
    sample::run_sample();
    sample::run_physics_sample();
    sample::run_blackboard_sample();
    run_benchmarks();

    return ts::test::summary();
}
