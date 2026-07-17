#pragma once

#include "ts/scheduler.h"

#include <thread>

namespace ts::detail
{

class Worker_thread
{
public:
    Worker_thread(Scheduler& scheduler, int index);

private:
    std::jthread thread_;

    static void main(Scheduler& scheduler, int index);
};

} // namespace ts::detail
