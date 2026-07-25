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

    // Arms (or disarms) stamping for the run starting now. Arms the scheduler's busy
    // tracking and snapshots its counter so the fold can attribute the run window's
    // executed work (core utilization); `fold` disarms.
    void begin_run(const Graph_trace* trace, Scheduler& scheduler)
    {
        tracing_ = trace != nullptr;
        if (tracing_)
        {
            scheduler_ = &scheduler;
            run_begin_ = now();
            // Bucket width comes from the trace's makespan estimate (0 on the first run, which
            // then goes un-bucketed -- negligible over a long trace); origin = this run's begin.
            long long bucket_width = trace->fixed_bucket_width_ticks(Scheduler::util_bucket_count);
            scheduler.arm_busy_tracking(run_begin_, bucket_width, static_cast<int>(start_.size()));
            busy_begin_ = scheduler.busy_ticks();
            tasks_begin_ = scheduler.task_count();
        }
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
    // thread. Skipped for cancelled runs (their stamps are partial). The busy delta is
    // every task the run's scheduler executed inside the window -- graph nodes, their
    // `parallel_for` slices, async pipe jobs, continuations -- which is the point of the
    // utilization metric; work run inline on non-worker threads is not counted.
    void fold(Graph_trace* trace, bool cancelled) const
    {
        if (!tracing_)
            return;
        long long busy_delta = scheduler_->busy_ticks() - busy_begin_;
        long long task_delta = scheduler_->task_count() - tasks_begin_;
        long long bucket_width = scheduler_->bucket_width();
        long long bucket_busy[Scheduler::util_bucket_count];
        scheduler_->read_bucket_busy(bucket_busy);
        int node_count = static_cast<int>(start_.size());
        std::vector<long long> owner_busy(static_cast<size_t>(node_count));   // per-node true busy (ticks)
        scheduler_->read_owner_busy(owner_busy.data(), node_count);
        scheduler_->disarm_busy_tracking();   // paired with begin_run's arm, cancelled or not
        if (!trace || cancelled)
            return;
        trace->on_run_complete(ready_.data(), start_.data(), end_.data(), worker_.data(),
            node_count, run_begin_, now(), busy_delta,
            scheduler_->worker_count(), bucket_busy, Scheduler::util_bucket_count, bucket_width,
            owner_busy.data(), task_delta);
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
    long long busy_begin_ = 0;
    long long tasks_begin_ = 0;
    Scheduler* scheduler_ = nullptr;   // non-const: fold disarms the busy tracking
    bool tracing_ = false;
#else
public:
    explicit Trace_stamps(size_t) {}
    void begin_run(const Graph_trace*, Scheduler&) {}
    void mark_ready(int) {}
    void mark_start(int) {}
    void mark_end(int) {}
    void fold(Graph_trace*, bool) const {}
#endif
};

} // namespace ts::tools
