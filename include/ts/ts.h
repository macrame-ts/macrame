#pragma once

// Umbrella header for the public API. Include this to get the whole library;
// or include the individual headers below if you only need part of it.
//
// Everything lives in namespace `ts` (internals in `ts::detail`).

#include "ts/version.h"            // TS_VERSION_* macros
#include "ts/scheduler.h"          // ts::Scheduler, Scheduler_config, Idle_policy, Priority
#include "ts/guarded.h"            // ts::Guarded<T>, ts::async (multi-object)
#include "ts/task.h"               // ts::Task<R>, ts::launch, Signal, cancellation
#include "ts/static_task_graph.h"  // ts::Static_task_graph, Graph_node
#include "ts/parallel_for.h"       // ts::parallel_for
#include "ts/deferred.h"           // ts::Deferred<T>
#include "ts/versioned.h"          // ts::Versioned<T>
#include "ts/access.h"             // TS_CHECK_ACCESS, Access_context (for instrumenting guarded types)
#include "ts/rules.h"              // ts::Rule, Relaxed_scope - the waiting-rule check policy

#include "ts/coroutine_support.h"  // co_await a Task, ts::read_only/ts::read_write access guards
