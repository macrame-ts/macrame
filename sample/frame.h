#pragma once

#include "ts/static_task_graph.h"
#include "world.h"

namespace sample
{

// Build and compile the per-frame task graph over a world. Ordering is derived
// from each system's declared store access (plus any explicit edges). Build once,
// execute() per frame.
ts::Static_task_graph build_frame_graph(World& world);

} // namespace sample
