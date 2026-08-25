// Dev-tooling helper, not part of the public API: RAII sugar over
// `create_scheduler`/`destroy_scheduler` for scope-bound use (tests, benchmarks, a sample
// tracing on a fixed worker count). On entry it brings the scheduler up with `config`; if one
// is already running it reconfigures - teardown + recreate - and restores the previous config
// on exit. Same coarse teardown+recreate semantics as the free functions; use at quiescent
// points only.
#pragma once

#include "ts/scheduler.h"

#include <optional>

namespace ts
{

class Scheduler_scope
{
public:
    explicit Scheduler_scope(Scheduler_config config)
    {
        if (scheduler_running())
        {
            prev_ = current_scheduler_config();   // reconfigure: restore this on exit
            destroy_scheduler();
        }
        create_scheduler(config);
    }

    ~Scheduler_scope()
    {
        destroy_scheduler();
        if (prev_)
            create_scheduler(*prev_);
    }

    Scheduler_scope(const Scheduler_scope&) = delete;
    Scheduler_scope& operator=(const Scheduler_scope&) = delete;

private:
    std::optional<Scheduler_config> prev_;   // the config to restore on exit, if one was running
};

} // namespace ts
