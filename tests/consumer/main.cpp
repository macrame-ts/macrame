// Consumer smoke test: proves the installed/added macrame library links and its public
// include interface works from OUTSIDE the tree - a real 5-line program against ts/ts.h.
#include "ts/ts.h"

#include <cstdio>

int main()
{
    ts::create_scheduler();

    ts::Guarded<int> counter{ ts::Named{ "counter" }, 0 };
    counter.access([](int& v) { v = 42; }).sync();
    int n = counter.access([](const int& v) { return v; }).sync();

    ts::Task<int> t = ts::launch([] { return 6 * 7; });
    int m = t.sync();

    ts::destroy_scheduler();

    std::printf("macrame consumer: counter=%d launch=%d\n", n, m);
    return (n == 42 && m == 42) ? 0 : 1;
}
