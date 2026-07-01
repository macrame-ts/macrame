#include "tests.h"
#include "benchmarks.h"
#include "harness.h"
#include "engine.h"

#include <cstring>

int main(int argc, char** argv)
{
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
    if (argc >= 2 && std::strcmp(argv[1], "--tests") == 0)
    {
        run_all_tests();
        return ts::test::summary();
    }

    run_all_tests();
    sample::run_sample();
    run_benchmarks();

    return ts::test::summary();
}
