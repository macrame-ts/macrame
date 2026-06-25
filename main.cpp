#include "tests.h"
#include "benchmarks.h"
#include "thread_safe_tests.h"

int main()
{
    run_tests();
    run_thread_safe_tests();
    run_benchmarks();
}
