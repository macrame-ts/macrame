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
// `trace_body_add(dt)` records `dt` ticks of USER-FUNCTOR time on the current worker: it
// adds to the worker's body accumulator (B) and subtracts from its machinery accumulator
// (M) -- so a functor's span moves from the default-machinery bucket into body. This is the
// task-system-cost split (B = user compute, M = scheduler machinery). Scheduler-free here.
extern std::atomic<int> trace_owner_armed;
extern void (*trace_owner_add)(int owner, long long dt);
extern void (*trace_body_add)(long long dt);   // B += dt, M -= dt (body booked under run_task)
extern void (*trace_body_only)(long long dt);  // B += dt only (inline body: no run_task M to net)
extern void (*trace_machinery_add)(long long dt);  // M += dt (per-run graph setup, no run_task span)
// Phase 1 (additive) of the subtraction-based four-way overhead breakdown: a DEDICATED
// orchestration accumulator, parallel to (not replacing) the summed-M machinery above.
// `Trace_setup_scope` routes its per-run `execute()` setup span here IN ADDITION to
// `trace_machinery_add`, so the summed-M metric stays intact while the four-way split gets a
// clean fourth bucket (framework work done OFF-worker, absent from `busy_ticks`). Scheduler-free.
extern void (*trace_orchestration_add)(long long dt);  // Orch += dt (per-run graph setup, off-worker)

inline thread_local int current_trace_owner = -1;
// True while this thread is inside a user functor (a `Trace_busy_scope`). The scheduler's
// `submit` reads it to reclassify submit-from-within-a-body (e.g. `parallel_for` slice
// dispatch) as machinery rather than body -- the fan-out submission is task-system cost.
inline thread_local bool current_in_functor = false;
// Set by `run_task` around the body it dispatches: the task's whole span is booked as
// machinery there, so this body's span must be netted back OUT of machinery (and into
// body). An inline-dispatched body bypasses `run_task`, so this is false for
// it -- it only adds to body (nothing to net). Captured-and-cleared per body scope, so a
// nested inline body inside a `run_task` body correctly reads false.
inline thread_local bool trace_body_under_run_task = false;

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
// `trace_body_add` (the task-system-cost split: B += span, M -= span), and (2) if the task
// has an owning node, attributes the span to that node's true-busy sink. It also flags the
// thread as in-functor for the scope, so `submit` can reclassify fan-out dispatch as
// machinery. Disarmed cost: one relaxed load + branch (no clock read). Placed INSIDE the
// owner scope so the owner is live when it attributes.
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
            // Capture whether run_task booked this span as machinery, then clear it so a
            // nested inline body (dispatched from inside this one) reads false.
            booked_ = trace_body_under_run_task;
            trace_body_under_run_task = false;
            t0_ = std::chrono::steady_clock::now().time_since_epoch().count();
        }
    }
    ~Trace_busy_scope()
    {
        if (active_)
        {
            long long dt = std::chrono::steady_clock::now().time_since_epoch().count() - t0_;
            current_in_functor = in_functor_prev_;
            trace_body_under_run_task = booked_;
            if (booked_)
            {
                if (trace_body_add)
                    trace_body_add(dt);                // B += dt, M -= dt (net out of run_task's span)
            }
            else if (trace_body_only)
                trace_body_only(dt);                   // B += dt only (inline body: no run_task span)
            if (owner_ >= 0 && trace_owner_add)
                trace_owner_add(owner_, dt);           // per-node true busy
        }
    }
    Trace_busy_scope(const Trace_busy_scope&) = delete;
    Trace_busy_scope& operator=(const Trace_busy_scope&) = delete;

private:
    bool active_ = false;
    bool in_functor_prev_ = false;
    bool booked_ = false;
    int owner_ = -1;
    long long t0_ = 0;
};

// Brackets a graph run's per-run setup + initial dispatch (link binding, node re-arm,
// indegree init, root dispatch) in `Static_task_graph::execute()`. That work runs on the
// calling thread inside NO `run_task` span, so it escaped the machinery accumulator entirely
// and scaled with node count -- this scope folds it in. While armed it (1) records its wall
// span as machinery via `trace_machinery_add`, and (2) flags the span as `run_task`-booked
// (`trace_body_under_run_task`), so any body dispatched INLINE within it (a `set_inline` root,
// or every node in worker-less mode where the whole frame drains serially here) nets its own
// span back OUT of this machinery span -- exactly as a body nets out of `run_task`'s span. So
// the setup span minus the inline bodies it contains = pure machinery. Disarmed cost: one
// relaxed load + branch (no clock read). Non-worker callers land in the scheduler's overflow
// lane (the frame loop / a test), workers in their own slot (a nested `execute()`).
class Trace_setup_scope
{
public:
    Trace_setup_scope() noexcept
    {
        if (trace_owner_armed.load(std::memory_order_relaxed) != 0)
        {
            active_ = true;
            prev_booked_ = trace_body_under_run_task;
            trace_body_under_run_task = true;   // inline bodies within net out of this span
            t0_ = std::chrono::steady_clock::now().time_since_epoch().count();
        }
    }
    ~Trace_setup_scope()
    {
        if (active_)
        {
            long long dt = std::chrono::steady_clock::now().time_since_epoch().count() - t0_;
            trace_body_under_run_task = prev_booked_;
            if (trace_machinery_add)
                trace_machinery_add(dt);           // summed-M (unchanged): M += dt
            if (trace_orchestration_add)
                trace_orchestration_add(dt);        // four-way (additive): Orch += dt
        }
    }
    Trace_setup_scope(const Trace_setup_scope&) = delete;
    Trace_setup_scope& operator=(const Trace_setup_scope&) = delete;

private:
    bool active_ = false;
    bool prev_booked_ = false;
    long long t0_ = 0;
};

#else   // TS_PROFILING == 0: everything compiles away

inline int trace_owner() noexcept { return -1; }
class Trace_owner_scope { public: explicit Trace_owner_scope(int) noexcept {} };
class Trace_busy_scope { public: Trace_busy_scope() noexcept {} };
class Trace_setup_scope { public: Trace_setup_scope() noexcept {} };

#endif

} // namespace ts::detail
