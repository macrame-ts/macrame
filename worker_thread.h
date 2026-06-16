#pragma once

#include "scheduler.h"

#include <thread>

namespace detail
{

class Worker_thread
{
public:
    explicit Worker_thread(Scheduler& scheduler);

private:
    std::jthread thread_;

    static void main(Scheduler& scheduler);
};

} // namespace detail
