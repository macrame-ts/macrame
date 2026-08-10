#pragma once

// The core-side seam for per-node "true busy" attribution (owner attribution), Phase B of
// the runtime trace. Kept tiny and out of the task/graph logic: the core path gets at most
// a one-line scoped guard; the accounting lives in the scheduler's armed sink (mirroring
// the per-worker busy counters) and the folding is tools-side. Every type here no-ops under
// `TS_PROFILING=0`, so the launch/execute logic carries no `#if`.
//
// This header is scheduler-free on purpose: `task.h` (the scheduler-independent task core)
// includes it, so it reaches the scheduler only through a bridge - the same indirection
// `submit_ready` uses. The scheduler layer installs `trace_owner_add` and bumps
// `trace_owner_armed`; nothing here names `Scheduler`.
//
// `current_trace_owner` = the graph-node index owning the task running on this thread
// (-1 = none). A node body sets it (`Trace_owner_scope`); sub-work launched from the body
// inherits it (the snapshot `ts::launch` already does for the access context), so a
// `parallel_for` slice or async job on any worker knows which node it belongs to.
// `Trace_busy_scope` measures its own span and, while armed, attributes it to that node -
// so a node's true cost = its body + its slices + its async fan-out.

// Canonical default (repeated idempotently by scheduler.h / static_task_graph.h). Must be
// set before the `#if` below, since task.h may include this header before either of those.
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
// `trace_body_add(dt)` records `dt` ticks of user-functor time on the current worker by adding
// to the worker's body accumulator (B). B is add-only: the overhead metric derives machinery by
// pure subtraction (`M = busy - B`, Phase 2), so there is no machinery accumulator to net a
// body span out of. Scheduler-free here.
extern std::atomic<int> trace_owner_armed;
extern void (*trace_owner_add)(int owner, long long dt);
extern void (*trace_body_add)(long long dt);   // B += dt (user functor; machinery = busy - B is derived)
// Orchestration accumulator (the off-worker fourth bucket of the four-way subtraction split):
// `Trace_setup_scope` routes the per-run `execute()` setup span here - framework work done off
// any worker's `run_task` span, so absent from `busy`. Booked only for a top-level `execute()`
// (the frame loop / a test), not a nested one called from inside a node body - a nested run's
// setup runs inside the enclosing node's `run_task` span and is captured by that node's busy/body
// accounting (see `Trace_setup_scope`). Scheduler-free.
extern void (*trace_orchestration_add)(long long dt);  // Orch += dt (top-level graph setup, off-worker)

inline thread_local int current_trace_owner = -1;
// True while this thread is inside a user functor (a `Trace_busy_scope`). `Trace_setup_scope`
// reads it to book orchestration only for a top-level `execute()` (not one nested inside a body).
inline thread_local bool current_in_functor = false;

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

// Brackets a user functor. While armed it (1) records its wall span as body time via
// `trace_body_add` (B += span; machinery is derived as `busy - B`, so nothing to net), and
// (2) if the task has an owning node, attributes the span to that node's true-busy sink. It
// also flags the thread as in-functor for the scope, so a nested `execute()`'s setup is
// recognized as in-body (and not double-booked as orchestration). Disarmed cost: one relaxed
// load + branch (no clock read). Placed inside the owner scope so the owner is live when it
// attributes.
class Trace_busy_scope
{
public:
    Trace_busy_scope() noexcept
    {
        if (trace_owner_armed.load(std::memory_order_relaxed) != 0)
        {
            active_ = true;
            owner_ = current_trace_owner;
            in_functor_prev_ = current_in_functor;
            current_in_functor = true;
            t0_ = std::chrono::steady_clock::now().time_since_epoch().count();
        }
    }
    ~Trace_busy_scope()
    {
        if (active_)
        {
            long long dt = std::chrono::steady_clock::now().time_since_epoch().count() - t0_;
            current_in_functor = in_functor_prev_;
            if (trace_body_add)
                trace_body_add(dt);                    // B += dt (machinery = busy - B is derived)
            if (owner_ >= 0 && trace_owner_add)
                trace_owner_add(owner_, dt);           // per-node true busy
        }
    }
    Trace_busy_scope(const Trace_busy_scope&) = delete;
    Trace_busy_scope& operator=(const Trace_busy_scope&) = delete;

private:
    bool active_ = false;
    bool in_functor_prev_ = false;
    int owner_ = -1;
    long long t0_ = 0;
};

// Brackets a graph run's per-run setup + initial dispatch (link binding, node re-arm,
// indegree init, root dispatch) in `Static_task_graph::execute()`. For a top-level run (the
// frame loop / a test) that work runs on the calling thread inside no `run_task` span, so it is
// absent from `busy` and would escape the four-way split - this scope books it to the dedicated
// orchestration bucket. For a nested run (an `execute()` called from inside a node body,
// `current_in_functor` true) the setup runs inside the enclosing node's `run_task` span and is
// already captured by that node's busy/body accounting; booking orchestration too would
// double-count it, so the scope stays silent when in-body. Disarmed cost: one relaxed load +
// branch (no clock read). The span lands in the scheduler's overflow lane (a top-level caller is
// a non-worker: the frame loop or a test).
class Trace_setup_scope
{
public:
    Trace_setup_scope() noexcept
    {
        // Only a top-level `execute()` books orchestration: a nested in-body run's setup is
        // already inside the enclosing node's span (busy/body), so skip it to avoid double count.
        if (!current_in_functor && trace_owner_armed.load(std::memory_order_relaxed) != 0)
        {
            active_ = true;
            t0_ = std::chrono::steady_clock::now().time_since_epoch().count();
        }
    }
    ~Trace_setup_scope()
    {
        if (active_)
        {
            long long dt = std::chrono::steady_clock::now().time_since_epoch().count() - t0_;
            if (trace_orchestration_add)
                trace_orchestration_add(dt);        // Orch += dt (top-level setup, off-worker)
        }
    }
    Trace_setup_scope(const Trace_setup_scope&) = delete;
    Trace_setup_scope& operator=(const Trace_setup_scope&) = delete;

private:
    bool active_ = false;
    long long t0_ = 0;
};

#else   // TS_PROFILING == 0: everything compiles away

inline int trace_owner() noexcept { return -1; }
class Trace_owner_scope { public: explicit Trace_owner_scope(int) noexcept {} };
class Trace_busy_scope { public: Trace_busy_scope() noexcept {} };
class Trace_setup_scope { public: Trace_setup_scope() noexcept {} };

#endif

} // namespace ts::detail
