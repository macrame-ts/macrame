#pragma once

#include "scheduler.h"

#include <thread>

namespace detail
{

class Worker_thread
{
public:
    Worker_thread(Scheduler& scheduler, int index);

private:
    std::jthread thread_;

    static void main(Scheduler& scheduler, int index);
};

} // namespace detail
