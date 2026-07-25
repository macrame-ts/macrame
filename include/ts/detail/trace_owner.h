#pragma once

// The core-side seam for per-node "true busy" attribution (owner attribution), Phase B of
// the runtime trace. Kept tiny and OUT of the task/graph logic: the core path gets at most
// a one-line scoped guard; the accounting lives in the scheduler's armed sink (mirroring
// the per-worker busy counters) and the folding is tools-side. Every type here no-ops under
// `TS_PROFILING=0`, so the launch/execute logic carries no `#if`.
//
// This header is SCHEDULER-FREE on purpose: `task.h` (the scheduler-independent task core)
// includes it, so it reaches the scheduler only through a bridge -- the same indirection
// `submit_ready` uses. The scheduler layer installs `trace_owner_add` and bumps
// `trace_owner_armed`; nothing here names `Scheduler`.
//
// `current_trace_owner` = the graph-node index owning the task running on this thread
// (-1 = none). A node body sets it (`Trace_owner_scope`); sub-work launched from the body
// inherits it (the snapshot `ts::launch` already does for the access context), so a
// `parallel_for` slice or async job on any worker knows which node it belongs to.
// `Trace_busy_scope` measures its own span and, while armed, attributes it to that node --
// so a node's true cost = its body + its slices + its async fan-out.

// Canonical default (repeated idempotently by scheduler.h / static_task_graph.h). Must be
// set BEFORE the `#if` below, since task.h may include this header before either of those.
#ifndef TS_PROFILING
#define TS_PROFILING 1
#endif

#if TS_PROFILING
#include <atomic>
#include <chrono>
#endif

namespace ts::detail
{

#if TS_PROFILING

// Bridge (defined + installed by the scheduler TU): armed != 0 while a traced run is in
// flight; `trace_owner_add(owner, dt)` adds `dt` ticks to node `owner`'s busy sink.
extern std::atomic<int> trace_owner_armed;
extern void (*trace_owner_add)(int owner, long long dt);

inline thread_local int current_trace_owner = -1;

inline int trace_owner() noexcept { return current_trace_owner; }

// Set the owning node for a scope (save/restore, so inline-nested runs and inherited
// sub-work restore correctly). A cheap TLS write; the attribution scope gates on the trace.
class Trace_owner_scope
{
public:
    explicit Trace_owner_scope(int owner) noexcept
        : prev_(current_trace_owner)
    {
        current_trace_owner = owner;
    }
    ~Trace_owner_scope() { current_trace_owner = prev_; }
    Trace_owner_scope(const Trace_owner_scope&) = delete;
    Trace_owner_scope& operator=(const Trace_owner_scope&) = delete;

private:
    int prev_;
};

// Measure this scope and attribute its wall span (busy) to the current owner node, via the
// bridge. Disarmed cost: one relaxed load + branch (no clock read). Placed INSIDE the owner
// scope so the owner is live when it attributes.
class Trace_busy_scope
{
public:
    Trace_busy_scope() noexcept
    {
        if (trace_owner_armed.load(std::memory_order_relaxed) != 0 && current_trace_owner >= 0)
        {
            active_ = true;
            t0_ = std::chrono::steady_clock::now().time_since_epoch().count();
        }
    }
    ~Trace_busy_scope()
    {
        if (active_ && trace_owner_add)
        {
            long long dt = std::chrono::steady_clock::now().time_since_epoch().count() - t0_;
            trace_owner_add(current_trace_owner, dt);
        }
    }
    Trace_busy_scope(const Trace_busy_scope&) = delete;
    Trace_busy_scope& operator=(const Trace_busy_scope&) = delete;

private:
    bool active_ = false;
    long long t0_ = 0;
};

#else   // TS_PROFILING == 0: everything compiles away

inline int trace_owner() noexcept { return -1; }
class Trace_owner_scope { public: explicit Trace_owner_scope(int) noexcept {} };
class Trace_busy_scope { public: Trace_busy_scope() noexcept {} };

#endif

} // namespace ts::detail
