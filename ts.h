#pragma once

// Umbrella header for the public API. Include this to get the whole library;
// or include the individual headers below if you only need part of it.
//
// Everything lives in namespace `ts` (internals in `ts::detail`).

#include "scheduler.h"          // ts::Scheduler, Scheduler_config, Idle_policy, Priority
#include "guarded.h"            // ts::Guarded<T>, ts::async (multi-object)
#include "task.h"               // ts::Task<R>, launch/task/nested, then/when_all, Signal, cancellation
#include "static_task_graph.h"  // ts::Static_task_graph, Graph_node
#include "parallel_for.h"       // ts::parallel_for
#include "deferred.h"           // ts::Deferred<T>
#include "versioned.h"          // ts::Versioned<T>
#include "access.h"             // TS_CHECK_ACCESS, Access_context (for instrumenting guarded types)

// Coroutine support is optional -- only pulled in when the compiler enables
// coroutines (it is header-only and self-contained otherwise).
#if defined(__cpp_impl_coroutine)
#include "coroutine_support.h"  // co_await a Task, ts::read_only/ts::read_write pipe guards
#endif
