#pragma once

// Deterministic allocation profiler: overrides global operator new/delete with counters
// and reports allocations/op + bytes/op for each public feature. Answers "how big a problem
// are heap allocations" independent of wall-clock noise. Invoked via `main --memprofile`.
void run_mem_profile();
