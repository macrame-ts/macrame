#include "tests.h"
#include "sample_tests.h"
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

    run_sample_tests();
    run_all_tests();
    sample::run_sample();
    run_benchmarks();

    return ts::test::summary();
}
