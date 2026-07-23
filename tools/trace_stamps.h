#pragma once

#include "ts/static_task_graph.h"   // TS_PROFILING default + the tools::Graph_trace forward declaration
#include "ts/scheduler.h"

#if TS_PROFILING
#include "graph_trace.h"
#include <chrono>
#endif

#include <vector>

namespace ts::tools
{

// The capture side of the graph's profiling seam (consumed by `static_task_graph.cpp`).
// Per-run trace stamps -- raw `steady_clock` ticks + executing worker per node, written
// only while a trace is attached (each node writes its own slot; the settle's acq_rel
// chain publishes them to the folding thread; sized once, no per-run allocation).
// `mark_ready` is the data-ready instant (dependency counter hit zero); the gap to
// `mark_start` is acquire + queue latency, attributed per node by the trace. Every method
// compiles to a no-op without `TS_PROFILING`, so the graph's run logic carries no `#if`s.
class Trace_stamps
{
#if TS_PROFILING
public:
    explicit Trace_stamps(size_t node_count)
        : ready_(node_count)
        , start_(node_count)
        , end_(node_count)
        , worker_(node_count)
    {}

    // Arms (or disarms) stamping for the run starting now.
    void begin_run(const Graph_trace* trace)
    {
        tracing_ = trace != nullptr;
        if (tracing_)
            run_begin_ = now();
    }

    void mark_ready(int index)
    {
        if (tracing_)
            ready_[static_cast<size_t>(index)] = now();
    }

    void mark_start(int index)
    {
        if (tracing_)
        {
            start_[static_cast<size_t>(index)] = now();
            worker_[static_cast<size_t>(index)] = current_worker_index;
        }
    }

    void mark_end(int index)
    {
        if (tracing_)
            end_[static_cast<size_t>(index)] = now();   // body + nested tasks have settled
    }

    // The one containment call: fold the run's stamps into the trace, on the settling
    // thread. Skipped for cancelled runs (their stamps are partial).
    void fold(Graph_trace* trace, bool cancelled) const
    {
        if (!tracing_ || !trace || cancelled)
            return;
        trace->on_run_complete(ready_.data(), start_.data(), end_.data(), worker_.data(),
            static_cast<int>(start_.size()), run_begin_, now());
    }

private:
    static long long now()
    {
        return std::chrono::steady_clock::now().time_since_epoch().count();
    }

    std::vector<long long> ready_;
    std::vector<long long> start_;
    std::vector<long long> end_;
    std::vector<int> worker_;
    long long run_begin_ = 0;
    bool tracing_ = false;
#else
public:
    explicit Trace_stamps(size_t) {}
    void begin_run(const Graph_trace*) {}
    void mark_ready(int) {}
    void mark_start(int) {}
    void mark_end(int) {}
    void fold(Graph_trace*, bool) const {}
#endif
};

} // namespace ts::tools
