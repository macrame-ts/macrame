#pragma once

#include "ts/priority.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace ts::tools
{

// Critical dead time -- makespan minus the work on the measured binding chain -- as a
// share of makespan classifies the frame. Below `dead_time_ok_share` the chain's next
// node almost never waits: the frame is dependency-bound, and only restructuring edges
// or shrinking critical nodes makes it faster (green). Above `dead_time_bad_share` the
// chain spends a significant share of the frame ready-but-not-running: scheduling-bound,
// where worker count, priorities, or ordering are the lever (red). Between the two is
// the caution band (yellow). The bands colour the headline dead-time line in
// `write_SVG`.
inline constexpr double dead_time_ok_share = 0.05;    // below: green (dependency-bound)
inline constexpr double dead_time_bad_share = 0.10;   // above: red (scheduling-bound)

// Core utilization -- the share of the run window the scheduler's workers spent executing
// tasks (busy delta / (workers x window); every task kind on the run's scheduler counts,
// work run inline on non-worker threads does not). What "good" means is bounded by the
// frame's critical path -- max(serial/workers, chain) caps the achievable share -- so the
// bands are deliberately forgiving: a frame using at least three quarters of its cores is
// green; under half, most core time is idle and the frame likely has parallelism (or
// worker-count) headroom -- red; between is the caution band (yellow). Colours the
// headline utilization figure in `write_SVG`, next to dead time: utilization says how
// much of the machine the frame used, dead time says whether the critical chain itself
// had to wait.
inline constexpr double core_util_good_share = 0.75;   // at/above: green
inline constexpr double core_util_ok_share = 0.50;     // at/above: yellow; below: red

// Dead-time cause classification (Scalasca-style wait states, from aggregates we already
// hold): each critical dead-time band is classified by the mean core utilization DURING
// its gap, joined from the time-bucketed utilization. Cores saturated while the chain
// waited = the chain waited for a CORE (core-bound -- capacity is the lever: add workers,
// shed off-path work). Cores idle = it waited for a DEPENDENCY or dispatch (dependency-
// bound -- cut/loosen the edge, overlap the producer, check wake latency). Between the
// two is mixed. The thresholds reuse the utilization bands above: the same figure that
// colours the utilization headline decides what a gap's utilization means.
inline constexpr double gap_core_bound_share = core_util_good_share;         // >=: core-bound
inline constexpr double gap_dependency_bound_share = core_util_ok_share;     // <: dependency-bound

// Task-system overhead -- the share of the frame's COMPUTE (body + machinery, excluding
// idle) spent in the scheduler's own machinery: task setup/completion, successful
// find_work scans, and submit fan-out issued from inside a body. M / (B + M). Low is the
// expected regime for realistic node granularity (bodies dwarf the ~sub-microsecond
// per-task machinery); a high figure means the graph is too fine-grained -- the tasks are
// smaller than the cost of scheduling them. Reported as an UPPER bound (the coarse per-task
// clock brackets charge their own read latency to machinery); cross-check vs the microbench
// ns/op. Colours the overhead headline figure in `write_SVG`.
inline constexpr double overhead_ok_share = 0.05;      // at/below: green (bodies dominate)
inline constexpr double overhead_bad_share = 0.15;     // above: red (too fine-grained)

// Critical-path ranking table (`write_SVG`, below the legend): nodes with a chain share
// below this cutoff are omitted from the table; a footer reports how many were dropped
// and the largest omitted share.
inline constexpr double rank_min_share = 0.05;

// Aggregated runtime trace for a `Static_task_graph`: attach with
// `graph.set_trace(&trace)` (after `compile()`), run any number of times, then
// `write_SVG(path)` renders the AVERAGE run -- node bars packed into anonymous
// concurrency rows (row occupancy over time = the average frame's concurrency; workers
// are interchangeable, so rows carry no worker identity), dependency edges between them,
// per-node stats in hover tooltips.
//
// No samples are retained: every statistic is streamed (Welford mean/variance, P^2
// quantile markers for P50/P95, min/max, a per-worker run histogram), so state is
// O(nodes + edges) regardless of run count. The graph folds one completed run into the
// trace per `execute()` (a single call at run settle, on the settling thread; runs are
// sequential, so no member here needs atomics). Cancelled runs are not folded.
//
// The drawn bars are median-based ([P50 start, P50 start + P50 duration]); medians are
// not linear, so an edge's bars can cross in the aggregate even though no real run had
// them overlap -- such an edge is resolved by clamping both bars to the streamed mean
// MEET POINT of the edge (the average of predecessor-end and successor-start). Bars that
// overlap in time land on different rows by construction (greedy interval packing).
class Graph_trace
{
public:
    // --- structure (pushed by `Static_task_graph::set_trace` / re-pushed by a recompile;
    //     `begin_structure` also resets all aggregates) ---

    void begin_structure(int node_count)
    {
        nodes_.assign(static_cast<size_t>(node_count), {});
        edges_.clear();
        in_edges_dirty_ = true;
        reset();
    }

    void set_node_label(int index, std::string label)
    {
        nodes_[static_cast<size_t>(index)].label = std::move(label);
    }

    void set_node_priority(int index, Priority priority)
    {
        nodes_[static_cast<size_t>(index)].priority = priority;
    }

    void add_node_access(int index, std::string object, bool write)
    {
        nodes_[static_cast<size_t>(index)].accesses.emplace_back(std::move(object), write);
    }

    void add_edge(int from, int to, bool explicit_ordering, std::string conflict)
    {
        edges_.push_back({ from, to, explicit_ordering, std::move(conflict), {}, {}, 0 });
        in_edges_dirty_ = true;
    }

    // --- samples ---

    // Fold one completed run: raw `steady_clock` ticks per node (parallel arrays, one slot
    // per node) plus the run's begin/end ticks. `readys` is the data-ready instant (the
    // dependency counter's zero transition); ready-to-start is acquire + queue latency.
    // `busy_ticks` is the scheduler's executed-task wall time inside the window and
    // `worker_count` its pool size -- together the run's core utilization. Called by the
    // graph once per run, single-threaded; every duration/offset converts to microseconds
    // here.
    // The utilization background samples each run into `n` fixed time buckets; the width is
    // set once from the first available makespan estimate (ticks), and 0 until one exists (the
    // first run goes un-bucketed -- negligible over a long trace). Fixing it keeps the buckets
    // comparable across runs for the per-bucket Welford.
    long long fixed_bucket_width_ticks(int n) const
    {
        if (fixed_bucket_width_ticks_ != 0)
            return fixed_bucket_width_ticks_;
        if (makespan_.n == 0 || n <= 0)
            return 0;
        fixed_bucket_width_ticks_ = static_cast<long long>((makespan_.mean / ticks_to_us) / n);
        return fixed_bucket_width_ticks_;
    }

    // `util_bucket_busy[0..util_bucket_count)` is the per-bucket busy (summed over workers,
    // ticks) for this run's window; `bucket_width_ticks` its bucket width (0 = not bucketed).
    void on_run_complete(const long long* readys, const long long* starts,
                         const long long* ends, const int* workers,
                         int node_count, long long run_begin, long long run_end,
                         long long busy_ticks, int worker_count,
                         const long long* util_bucket_busy = nullptr,
                         int util_bucket_count = 0, long long bucket_width_ticks = 0,
                         const long long* owner_busy = nullptr, long long task_count = 0,
                         long long body_ticks = 0, long long orchestration_ticks = 0)
    {
        if (node_count != static_cast<int>(nodes_.size()))
            return;   // structure not pushed (or a stale attach) -- drop the sample

        long long window = run_end - run_begin;
        if (worker_count > 0 && window > 0)
        {
            core_util_.add(std::clamp(
                static_cast<double>(busy_ticks) / (static_cast<double>(worker_count) * static_cast<double>(window)),
                0.0, 1.0));
        }

        // True per-time utilization: per-bucket busy / (workers x bucket width), Welford per
        // bucket across runs. Counts every task kind (slices, async, ...) at its real time,
        // so a parallel_for node's core fan-out registers -- unlike the old node-concurrency
        // proxy this replaces.
        if (util_bucket_busy && util_bucket_count > 0 && bucket_width_ticks > 0 && worker_count > 0)
        {
            if (static_cast<int>(util_buckets_.size()) != util_bucket_count)
                util_buckets_.assign(static_cast<size_t>(util_bucket_count), {});
            util_bucket_width_us_ = static_cast<double>(bucket_width_ticks) * ticks_to_us;
            double denom = static_cast<double>(worker_count) * static_cast<double>(bucket_width_ticks);
            for (int b = 0; b < util_bucket_count; ++b)
            {
                util_buckets_[static_cast<size_t>(b)].add(
                    std::clamp(static_cast<double>(util_bucket_busy[b]) / denom, 0.0, 1.0));
            }
        }

        for (int i = 0; i < node_count; ++i)
        {
            Node_agg& a = nodes_[static_cast<size_t>(i)];
            double s = static_cast<double>(starts[i] - run_begin) * ticks_to_us;
            double d = static_cast<double>(ends[i] - starts[i]) * ticks_to_us;
            if (a.duration.n == 0)
            {
                a.min_dur = d;
                a.max_dur = d;
            }
            a.min_dur = std::min(a.min_dur, d);
            a.max_dur = std::max(a.max_dur, d);
            a.duration.add(d);
            a.start.add(s);
            a.dur_P50.add(d);
            a.dur_P95.add(d);
            a.start_P50.add(s);
            a.dispatch_wait.add(std::max(0.0, static_cast<double>(starts[i] - readys[i]) * ticks_to_us));
            // True busy = this node's body + its parallel_for slices + its async fan-out,
            // attributed via the scheduler's owner sink (now that parallel_for fans out on
            // the run's scheduler). 0 when unavailable (e.g. work run inline on a non-worker
            // thread, which the sink doesn't see).
            if (owner_busy)
                a.true_busy.add(static_cast<double>(owner_busy[i]) * ticks_to_us);
            int w = workers[i];
            if (w < 0)
            {
                ++a.external_runs;
            }
            else
            {
                if (w >= static_cast<int>(a.worker_runs.size()))
                    a.worker_runs.resize(static_cast<size_t>(w) + 1, 0);
                ++a.worker_runs[static_cast<size_t>(w)];
            }
        }

        for (Edge_agg& e : edges_)
        {
            double meet = (static_cast<double>(ends[e.from] - run_begin)
                         + static_cast<double>(starts[e.to] - run_begin)) * 0.5 * ticks_to_us;
            e.meet.add(meet);
        }

        fold_critical_chain(starts, ends, node_count);

        double mk = static_cast<double>(run_end - run_begin) * ticks_to_us;
        if (runs_ == 0)
        {
            makespan_min_ = mk;
            makespan_max_ = mk;
        }
        makespan_min_ = std::min(makespan_min_, mk);
        makespan_max_ = std::max(makespan_max_, mk);
        makespan_.add(mk);

        // Task volume: every task the scheduler ran in the window (nodes + parallel_for
        // slices + async + continuations), so it far exceeds the node count -- the real
        // fan-out. Total across the trace + mean per run.
        tasks_total_ += task_count;
        tasks_per_run_.add(static_cast<double>(task_count));

        // Task-system cost, PURE SUBTRACTION (Phase 2): in core-time T = workers x makespan
        // every worker-moment is body, framework-machinery, or idle: T = B + M + P, so machinery
        // is DERIVED, `M = busy - B` (busy = the run_task span sum + successful find_work scans),
        // and idle `P = T - busy`. There is no machinery accumulator. Framework work done
        // OFF-worker (the per-run top-level execute() setup on the frame-loop thread) is absent
        // from busy, so it is a separate fourth bucket, Orchestration, with its own accumulator.
        // Overhead = M / (B + M) (idle excluded -- a clean compute-cost split). Fed only for a
        // multi-worker run (busy/T are meaningful only then); the worker-less oracle reads B
        // directly off the scheduler, not through this fold.
        if (worker_count > 0 && window > 0)
        {
            double t_us = static_cast<double>(worker_count) * static_cast<double>(window) * ticks_to_us;
            double busy_us = static_cast<double>(busy_ticks) * ticks_to_us;
            double b_us = static_cast<double>(body_ticks) * ticks_to_us;
            double m_us = busy_us - b_us;                        // M = busy - B (derived machinery)
            double idle_us = t_us - busy_us;                     // P = T - busy
            double orch_us = static_cast<double>(orchestration_ticks) * ticks_to_us;
            body_us_.add(b_us);
            machinery_us_.add(m_us);
            four_T_us_.add(t_us);
            four_idle_us_.add(idle_us);
            four_orch_us_.add(orch_us);
        }

        ++runs_;
    }

    // Clear aggregates (structure stays).
    void reset()
    {
        for (Node_agg& a : nodes_)
        {
            a.duration = {};
            a.start = {};
            a.dur_P50 = { 0.50 };
            a.dur_P95 = { 0.95 };
            a.start_P50 = { 0.50 };
            a.min_dur = 0.0;
            a.max_dur = 0.0;
            a.worker_runs.clear();
            a.external_runs = 0;
            a.dispatch_wait = {};
            a.true_busy = {};
            a.critical_runs = 0;
        }
        for (Edge_agg& e : edges_)
        {
            e.meet = {};
            e.binding_gap = {};
            e.critical_runs = 0;
        }
        tasks_total_ = 0;
        tasks_per_run_ = {};
        body_us_ = {};
        machinery_us_ = {};
        four_T_us_ = {};
        four_idle_us_ = {};
        four_orch_us_ = {};
        critical_work_ = {};
        makespan_ = {};
        makespan_min_ = 0.0;
        makespan_max_ = 0.0;
        core_util_ = {};
        util_buckets_.clear();
        util_bucket_width_us_ = 0.0;
        fixed_bucket_width_ticks_ = 0;
        runs_ = 0;
    }

    long long run_count() const { return runs_; }
    int structure_node_count() const { return static_cast<int>(nodes_.size()); }
    long long task_total() const { return tasks_total_; }        // tasks (every kind) across the trace
    double tasks_per_run() const { return tasks_per_run_.mean; }  // mean tasks per run

    // Task-system overhead, [0,1]: mean machinery / (mean body + mean machinery) over the folded
    // runs, where machinery `M = busy - B` is derived by PURE SUBTRACTION (Phase 2) -- it captures
    // task setup/completion, successful find_work scans, and on-worker inline machinery, all inside
    // `busy`; the per-run top-level graph setup is the separate off-worker Orchestration bucket. An
    // UPPER bound on scheduler cost -- the coarse per-task clock brackets charge their own read
    // latency to the body span (and so shrink M). Cross-check against the worker-less ground truth
    // (`set_ground_truth_overhead`) and the microbench ns/op.
    double body_us() const { return body_us_.mean; }
    double machinery_us() const { return machinery_us_.mean; }
    double overhead() const
    {
        double b = body_us_.mean, m = machinery_us_.mean;
        return (b + m) > 0.0 ? m / (b + m) : 0.0;
    }

    // --- Four-way subtraction breakdown ------------------------------------------------
    // Shares of core-time T = workers x makespan: body / machinery(M = busy - B) / idle(T - busy) /
    // orchestration (the off-worker per-run top-level execute() setup). These four are a partition of
    // T by construction (body + M + idle = busy + idle = T exactly, and orchestration is the extra
    // off-worker framework slice T does not otherwise see). Reported as mean-component / mean-T.
    double four_way_body_share() const
    {
        return four_T_us_.mean > 0.0 ? body_us_.mean / four_T_us_.mean : 0.0;
    }
    double four_way_machinery_share() const
    {
        return four_T_us_.mean > 0.0 ? machinery_us_.mean / four_T_us_.mean : 0.0;
    }
    double four_way_idle_share() const
    {
        return four_T_us_.mean > 0.0 ? four_idle_us_.mean / four_T_us_.mean : 0.0;
    }
    double four_way_orchestration_share() const
    {
        return four_T_us_.mean > 0.0 ? four_orch_us_.mean / four_T_us_.mean : 0.0;
    }
    // Per-run means (µs) of the four-way components.
    double four_way_body_us() const { return body_us_.mean; }
    double four_way_machinery_us() const { return machinery_us_.mean; }
    double four_way_idle_us() const { return four_idle_us_.mean; }
    double four_way_orchestration_us() const { return four_orch_us_.mean; }
    double four_way_core_time_us() const { return four_T_us_.mean; }

    // Mean core utilization over the folded runs, [0,1] (see the band constants above).
    double core_utilization() const { return core_util_.mean; }

    // Per-node aggregates (microseconds), for reports and tests.
    struct Node_stats
    {
        long long runs = 0;
        double mean_us = 0.0, P50_us = 0.0, P95_us = 0.0, stddev_us = 0.0;
        double min_us = 0.0, max_us = 0.0;
        double start_p50_us = 0.0;
        int modal_worker = -1;          // -1 = external (non-worker) threads
        double off_modal = 0.0;         // share of runs NOT on the modal worker
        double critical_share = 0.0;    // share of runs on the measured binding chain
        double dispatch_wait_us = 0.0;  // mean ready-to-start latency (acquire + queue)
        double true_busy_us = 0.0;      // mean body + slices + async fan-out (owner sink)
    };

    // Aggregates for one node. Well-defined for ANY index: an out-of-range one (including
    // every index on a trace that was never populated -- which is what a `TS_PROFILING=0`
    // build produces, since the structure export compiles to nothing) yields a zeroed
    // `Node_stats`, whose `runs == 0` is the honest "no data". A stubbed-out tool asked for
    // data must answer, not read past the end of a vector; the previous unchecked index was
    // a live out-of-bounds read that crashed the Shipping suite.
    Node_stats node_stats(int index) const
    {
        if (index < 0 || index >= static_cast<int>(nodes_.size()))
            return {};
        const Node_agg& a = nodes_[static_cast<size_t>(index)];
        Node_stats s;
        s.runs = a.duration.n;
        s.mean_us = a.duration.mean;
        s.P50_us = a.dur_P50.value();
        s.P95_us = a.dur_P95.value();
        s.stddev_us = a.duration.stddev();
        s.min_us = a.min_dur;
        s.max_us = a.max_dur;
        s.start_p50_us = a.start_P50.value();
        long long modal_count = 0;
        modal_count = modal(a, s.modal_worker);
        s.off_modal = a.duration.n > 0
            ? 1.0 - static_cast<double>(modal_count) / static_cast<double>(a.duration.n) : 0.0;
        s.critical_share = runs_ > 0
            ? static_cast<double>(a.critical_runs) / static_cast<double>(runs_) : 0.0;
        s.dispatch_wait_us = a.dispatch_wait.mean;
        s.true_busy_us = a.true_busy.mean;
        return s;
    }

    // Optional title prefix for the rendered SVG (e.g. `Sample "game_frame"` -> the
    // header reads `Sample "game_frame": average run`).
    void set_title(std::string title)
    {
        title_ = std::move(title);
    }

    // Attach the worker-less ground-truth overhead (the oracle) so `write_SVG` prints it next to
    // the `M = busy - B` overhead metric. Measured on a serial single-thread run of the SAME frame
    // as `(total_wall - body) / total_wall` -- the complete framework cost by pure subtraction, no
    // idle to confound it, so it is the per-op framework FLOOR. The gap `overhead() - this` on a
    // multi-worker run is the machinery only workers pay (cross-thread dispatch, pipe hand-off,
    // park/wake) -- not a metric blind spot. Negative = unset. Reported as-is, not matched.
    void set_ground_truth_overhead(double complement) { ground_truth_overhead_ = complement; }

    // Render the average run; returns false (reported to stderr) on I/O failure.
    bool write_SVG(const char* path) const;

private:
    // Streaming mean/variance (Welford).
    struct Welford
    {
        long long n = 0;
        double mean = 0.0;
        double m2 = 0.0;

        void add(double x)
        {
            ++n;
            double d = x - mean;
            mean += d / static_cast<double>(n);
            m2 += d * (x - mean);
        }
        double stddev() const { return n > 1 ? std::sqrt(m2 / static_cast<double>(n - 1)) : 0.0; }
    };

    // P^2 quantile estimator (Jain & Chlamtac, 1985): five markers track the p-quantile
    // in O(1) per sample, no samples stored. Below five samples, nearest rank on the
    // buffered few.
    struct P2
    {
        double p = 0.5;
        double q[5]{};
        double n[5]{};
        double np[5]{};
        double dn[5]{};
        int count = 0;

        void add(double x)
        {
            if (count < 5)
            {
                q[count++] = x;
                if (count == 5)
                {
                    std::sort(q, q + 5);
                    for (int i = 0; i < 5; ++i)
                        n[i] = i + 1;
                    np[0] = 1; np[1] = 1 + 2 * p; np[2] = 1 + 4 * p; np[3] = 3 + 2 * p; np[4] = 5;
                    dn[0] = 0; dn[1] = p / 2; dn[2] = p; dn[3] = (1 + p) / 2; dn[4] = 1;
                }
                return;
            }

            int k;
            if (x < q[0]) { q[0] = x; k = 0; }
            else if (x >= q[4]) { q[4] = x; k = 3; }
            else { k = 0; while (k < 3 && x >= q[k + 1]) ++k; }

            for (int i = k + 1; i < 5; ++i)
                n[i] += 1;
            for (int i = 0; i < 5; ++i)
                np[i] += dn[i];

            for (int i = 1; i <= 3; ++i)
            {
                double d = np[i] - n[i];
                if ((d >= 1 && n[i + 1] - n[i] > 1) || (d <= -1 && n[i - 1] - n[i] < -1))
                {
                    int s = d >= 0 ? 1 : -1;
                    double qp = parabolic(i, s);
                    if (!(q[i - 1] < qp && qp < q[i + 1]))
                        qp = q[i] + s * (q[i + s] - q[i]) / (n[i + s] - n[i]);   // linear fallback
                    q[i] = qp;
                    n[i] += s;
                }
            }
            ++count;
        }

        double parabolic(int i, int s) const
        {
            return q[i] + s / (n[i + 1] - n[i - 1])
                * ((n[i] - n[i - 1] + s) * (q[i + 1] - q[i]) / (n[i + 1] - n[i])
                 + (n[i + 1] - n[i] - s) * (q[i] - q[i - 1]) / (n[i] - n[i - 1]));
        }

        double value() const
        {
            if (count == 0)
                return 0.0;
            if (count <= 5)
            {
                double tmp[5];
                std::copy(q, q + count, tmp);
                std::sort(tmp, tmp + count);
                int idx = std::clamp(static_cast<int>(std::lround(p * (count - 1))), 0, count - 1);
                return tmp[idx];
            }
            return q[2];
        }
    };

    struct Node_agg
    {
        std::string label;
        Priority priority = Priority::normal;
        std::vector<std::pair<std::string, bool>> accesses;   // object label, write?
        Welford duration;    // µs
        Welford start;       // µs offset from run begin
        P2 dur_P50{ 0.50 };
        P2 dur_P95{ 0.95 };
        P2 start_P50{ 0.50 };
        double min_dur = 0.0;
        double max_dur = 0.0;
        std::vector<long long> worker_runs;   // runs per worker index
        long long external_runs = 0;          // runs on non-worker threads
        Welford dispatch_wait;                // ready-to-start latency, µs
        Welford true_busy;                    // body + slices + async fan-out, µs (owner sink)
        long long critical_runs = 0;          // runs where this node was on the binding chain
    };

    struct Edge_agg
    {
        int from = -1;
        int to = -1;
        bool explicit_ordering = false;
        std::string conflict;      // "obj: W->R; ..." or empty
        Welford meet;              // mean of (end_from + start_to)/2, µs from run begin
        Welford binding_gap;       // start_to - end_from on runs where this edge bound the chain, µs
        long long critical_runs = 0;   // runs where this edge was on the binding chain
    };

    static constexpr double ticks_to_us =
        1e6 * static_cast<double>(std::chrono::steady_clock::period::num)
            / static_cast<double>(std::chrono::steady_clock::period::den);

    // The run's measured critical path -- the chain that actually bound the makespan.
    // Walk backward from the latest-finishing node; at each step the BINDING predecessor
    // is the incoming-edge node that finished last (its completion released this node).
    // One counter per node and edge on the chain; a single run has one chain, but across
    // runs different chains bind, so the aggregate is a FREQUENCY, not a single path.
    // Distinct from the structural (CPM) path in `write_SVG`, which sees only durations
    // and edges -- the measured chain absorbs queue latency and pipe contention too.
    void fold_critical_chain(const long long* starts, const long long* ends, int node_count)
    {
        if (node_count == 0)
            return;
        if (in_edges_dirty_)
        {
            in_edges_.assign(nodes_.size(), {});
            for (size_t e = 0; e < edges_.size(); ++e)
                in_edges_[static_cast<size_t>(edges_[e].to)].push_back(static_cast<int>(e));
            in_edges_dirty_ = false;
        }

        int cur = 0;
        for (int i = 1; i < node_count; ++i)
        {
            if (ends[i] > ends[cur])
                cur = i;
        }

        double work = 0.0;
        while (cur >= 0)
        {
            ++nodes_[static_cast<size_t>(cur)].critical_runs;
            work += static_cast<double>(ends[cur] - starts[cur]) * ticks_to_us;

            int binding_edge = -1;
            for (int e : in_edges_[static_cast<size_t>(cur)])
            {
                if (binding_edge < 0
                    || ends[edges_[static_cast<size_t>(e)].from] > ends[edges_[static_cast<size_t>(binding_edge)].from])
                    binding_edge = e;
            }
            if (binding_edge < 0)
                break;   // a root: the chain is complete
            Edge_agg& be = edges_[static_cast<size_t>(binding_edge)];
            ++be.critical_runs;
            // The chain's dead time at this step: the successor sat ready-but-waiting
            // (queue / acquire) after its binding predecessor finished.
            be.binding_gap.add(std::max(0.0, static_cast<double>(starts[cur] - ends[be.from]) * ticks_to_us));
            cur = be.from;
        }
        critical_work_.add(work);
    }

    // Modal worker: the one that ran this node most often (or -1 = external). Returns
    // the modal count.
    static long long modal(const Node_agg& a, int& worker)
    {
        long long best = a.external_runs;
        worker = -1;
        for (size_t w = 0; w < a.worker_runs.size(); ++w)
        {
            if (a.worker_runs[w] > best)
            {
                best = a.worker_runs[w];
                worker = static_cast<int>(w);
            }
        }
        return best;
    }

    static void append_escaped(std::string& out, std::string_view text)
    {
        for (char c : text)
        {
            switch (c)
            {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c);
            }
        }
    }

    static std::string fmt_us(double us)
    {
        char buf[48];
        std::snprintf(buf, sizeof buf, us >= 100.0 ? "%.0f" : "%.1f", us);
        return buf;
    }

    // Frame-scale value in milliseconds (2 decimals) -- reads more naturally than µs for
    // the whole-frame figure.
    static std::string fmt_ms(double us)
    {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%.2f", us / 1000.0);
        return buf;
    }

    // Priority display: the node NAME takes the priority colour (high = warm red --
    // distinct from the critical pink (#f92672) and the critical-node orange (#fd971f) --
    // normal = green, low = grey). Criticality stays on the bar border and label weight.
    static const char* priority_color(Priority p)
    {
        // low grey brightened to the midpoint toward white so low-priority node text
        // (bar label + tooltip name) stays clearly legible on the dark ground.
        return p == Priority::high ? "#ff5f45" : p == Priority::low ? "#bab8ad" : "#a6e22e";
    }
    static const char* priority_word(Priority p)
    {
        return p == Priority::high ? "high" : p == Priority::low ? "low" : "normal";
    }

    // Criticality ramp shared by node borders/labels and edge colours: no effect under a
    // 10% share of binding chains, full effect from 80% up, linear between.
    static double crit_ramp(double share)
    {
        return share < 0.10 ? 0.0 : std::min(1.0, (share - 0.10) / 0.70);
    }

    // Linear colour blend for the criticality ramps.
    static std::string blend_hex(int r0, int g0, int b0, int r1, int g1, int b1, double f)
    {
        char buf[8];
        std::snprintf(buf, sizeof buf, "#%02x%02x%02x",
            static_cast<int>(std::lround(r0 + (r1 - r0) * f)),
            static_cast<int>(std::lround(g0 + (g1 - g0) * f)),
            static_cast<int>(std::lround(b0 + (b1 - b0) * f)));
        return buf;
    }

    std::vector<Node_agg> nodes_;
    std::vector<Edge_agg> edges_;
    std::vector<std::vector<int>> in_edges_;   // edge indices by target, for the chain walk
    bool in_edges_dirty_ = true;
    Welford critical_work_;   // per-run sum of chain node durations, µs
    Welford makespan_;
    double makespan_min_ = 0.0;
    double makespan_max_ = 0.0;
    Welford core_util_;   // per-run busy / (workers x window), [0,1]
    std::vector<Welford> util_buckets_;      // per time-bucket true utilization, [0,1]
    double util_bucket_width_us_ = 0.0;      // width of each util bucket, µs (0 = none yet)
    mutable long long fixed_bucket_width_ticks_ = 0;   // fixed once from the makespan estimate
    long long runs_ = 0;
    long long tasks_total_ = 0;   // total tasks (every kind) run across the trace
    Welford tasks_per_run_;       // tasks per run
    Welford body_us_;             // per-run user-functor wall time (B), summed over workers, µs
    Welford machinery_us_;        // per-run scheduler machinery, M = busy - B (derived), µs
    // Four-way subtraction breakdown: per-run core-time T = workers x makespan and its partition --
    // body (`body_us_`), machinery M = busy - B (`machinery_us_`), idle = T - busy, and the
    // off-worker orchestration slice. Shares taken vs mean T.
    Welford four_T_us_;           // per-run core-time (workers x makespan), µs
    Welford four_idle_us_;        // per-run idle P = T - busy, µs
    Welford four_orch_us_;        // per-run orchestration (off-worker top-level setup), µs
    double ground_truth_overhead_ = -1.0;   // worker-less (total-B)/total oracle; <0 = unset
    std::string title_;   // survives reset()/begin_structure(); set once by the owner
};

// --- the average-run renderer ---------------------------------------------------------

inline bool Graph_trace::write_SVG(const char* path) const
{
    const int count = static_cast<int>(nodes_.size());

    // Drawn bars: median start / median duration.
    std::vector<double> bar_start(static_cast<size_t>(count)), bar_end(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        const Node_agg& a = nodes_[static_cast<size_t>(i)];
        bar_start[static_cast<size_t>(i)] = std::max(0.0, a.start_P50.value());
        bar_end[static_cast<size_t>(i)] = bar_start[static_cast<size_t>(i)] + std::max(0.0, a.dur_P50.value());
    }

    // Topological order (Kahn over the pushed edges), for the meet-point clamp: medians
    // are not linear, so an edge's bars can cross in the aggregate; clamp both to the
    // edge's mean meet point, predecessors first so adjustments propagate.
    std::vector<std::vector<int>> out_edges(static_cast<size_t>(count));
    std::vector<int> indegree(static_cast<size_t>(count), 0);
    for (size_t e = 0; e < edges_.size(); ++e)
    {
        out_edges[static_cast<size_t>(edges_[e].from)].push_back(static_cast<int>(e));
        ++indegree[static_cast<size_t>(edges_[e].to)];
    }
    std::vector<int> topo;
    topo.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        if (indegree[static_cast<size_t>(i)] == 0)
            topo.push_back(i);
    }
    for (size_t h = 0; h < topo.size(); ++h)
    {
        for (int e : out_edges[static_cast<size_t>(topo[h])])
        {
            if (--indegree[static_cast<size_t>(edges_[static_cast<size_t>(e)].to)] == 0)
                topo.push_back(edges_[static_cast<size_t>(e)].to);
        }
    }

    // Structural critical path (CPM) over the average frame: forward/backward pass with
    // the same median durations the bars use, zero edge latency. This is the dependency
    // lower bound -- unlike the measured binding chain (fold_critical_chain), it cannot
    // see queue latency or pipe contention; the gap between the two classifies the frame
    // as dependency-bound or scheduling-bound. `slack` is how far a node can slip in the
    // average frame before it extends the CPM length.
    std::vector<double> dur(static_cast<size_t>(count)), est(static_cast<size_t>(count), 0.0);
    for (int i = 0; i < count; ++i)
        dur[static_cast<size_t>(i)] = std::max(0.0, nodes_[static_cast<size_t>(i)].dur_P50.value());
    for (int u : topo)
    {
        for (int e : out_edges[static_cast<size_t>(u)])
        {
            size_t v = static_cast<size_t>(edges_[static_cast<size_t>(e)].to);
            est[v] = std::max(est[v], est[static_cast<size_t>(u)] + dur[static_cast<size_t>(u)]);
        }
    }
    double cpm_us = 0.0;
    for (int i = 0; i < count; ++i)
        cpm_us = std::max(cpm_us, est[static_cast<size_t>(i)] + dur[static_cast<size_t>(i)]);
    std::vector<double> lf(static_cast<size_t>(count), cpm_us);
    for (size_t h = topo.size(); h-- > 0;)
    {
        size_t u = static_cast<size_t>(topo[h]);
        for (int e : out_edges[u])
        {
            size_t v = static_cast<size_t>(edges_[static_cast<size_t>(e)].to);
            lf[u] = std::min(lf[u], lf[v] - dur[v]);
        }
    }
    std::vector<double> slack(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        slack[static_cast<size_t>(i)] = std::max(0.0,
            lf[static_cast<size_t>(i)] - (est[static_cast<size_t>(i)] + dur[static_cast<size_t>(i)]));
    }

    // Critical-path ranking (the table below the legend): nodes ranked by expected chain
    // contribution -- chain share x mean duration, descending -- so a briefly-critical
    // heavy node outranks an always-critical trivial one. `dead_in` is the node's mean
    // incoming critical dead time: over its in-edges that ever bound the chain, the
    // binding-gap means weighted by how often each edge bound
    // (sum(gap_mean x edge.critical_runs) / sum(edge.critical_runs)) -- "when this node's
    // release was the chain's step, how long did it sit ready-but-waiting on average".
    // Negative = the node was never released by a binding edge (a root).
    struct Rank_row
    {
        int node = -1;
        double share = 0.0;
        double key = 0.0;       // share x mean duration, µs
        double dead_in = -1.0;  // µs; < 0 = no binding in-edge
    };
    std::vector<Rank_row> ranking;
    int rank_omitted = 0;
    int rank_max_omitted = -1;
    double rank_max_omitted_share = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const Node_agg& a = nodes_[static_cast<size_t>(i)];
        double share = runs_ > 0
            ? static_cast<double>(a.critical_runs) / static_cast<double>(runs_) : 0.0;
        if (share < rank_min_share)
        {
            ++rank_omitted;
            if (share > rank_max_omitted_share)
            {
                rank_max_omitted_share = share;
                rank_max_omitted = i;
            }
            continue;
        }
        Rank_row r;
        r.node = i;
        r.share = share;
        r.key = share * a.duration.mean;
        double gap_weighted = 0.0;
        long long gap_runs = 0;
        for (const Edge_agg& e : edges_)
        {
            if (e.to == i && e.critical_runs > 0)
            {
                gap_weighted += e.binding_gap.mean * static_cast<double>(e.critical_runs);
                gap_runs += e.critical_runs;
            }
        }
        if (gap_runs > 0)
            r.dead_in = gap_weighted / static_cast<double>(gap_runs);
        ranking.push_back(r);
    }
    std::sort(ranking.begin(), ranking.end(),
        [](const Rank_row& a, const Rank_row& b){ return a.key > b.key; });

    for (int u : topo)
    {
        for (int e : out_edges[static_cast<size_t>(u)])
        {
            const Edge_agg& edge = edges_[static_cast<size_t>(e)];
            size_t v = static_cast<size_t>(edge.to);
            if (bar_end[static_cast<size_t>(u)] > bar_start[v])
            {
                double m = edge.meet.mean;
                bar_end[static_cast<size_t>(u)] = std::min(bar_end[static_cast<size_t>(u)], m);
                bar_start[static_cast<size_t>(u)] = std::min(bar_start[static_cast<size_t>(u)], bar_end[static_cast<size_t>(u)]);
                bar_start[v] = std::max(bar_start[v], m);
                bar_end[v] = std::max(bar_end[v], bar_start[v]);
            }
        }
    }

    // Concurrency rows: workers are interchangeable and the node->worker assignment
    // reshuffles every run, so per-worker lanes misrepresent the aggregate -- a lane can
    // look idle while every real run had that worker busy, with its nodes drawn on their
    // OWN modal lanes. Instead all bars pack greedily into anonymous rows by interval
    // overlap (bar-start order; first row whose last bar has ended, else a new row). Row
    // occupancy over time IS the average frame's concurrency: free vertical space means
    // genuinely unused capacity in the average, not a projection artifact. The per-worker
    // histogram survives for stats (off-modal share, external runs); it no longer drives
    // layout.
    int workers_seen = 0;
    for (int i = 0; i < count; ++i)
        workers_seen = std::max(workers_seen, static_cast<int>(nodes_[static_cast<size_t>(i)].worker_runs.size()));

    std::vector<int> pack_order(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        pack_order[static_cast<size_t>(i)] = i;
    std::sort(pack_order.begin(), pack_order.end(), [&](int a, int b)
    {
        return bar_start[static_cast<size_t>(a)] < bar_start[static_cast<size_t>(b)];
    });
    std::vector<int> row_of(static_cast<size_t>(count), 0);
    std::vector<double> row_last_end;
    for (int nidx : pack_order)
    {
        size_t r = 0;
        while (r < row_last_end.size() && bar_start[static_cast<size_t>(nidx)] < row_last_end[r] - 1e-9)
            ++r;
        if (r == row_last_end.size())
            row_last_end.push_back(0.0);
        row_last_end[r] = bar_end[static_cast<size_t>(nidx)];
        row_of[static_cast<size_t>(nidx)] = static_cast<int>(r);
    }
    const int row_count = std::max<int>(1, static_cast<int>(row_last_end.size()));

    // Geometry.
    double span_us = 1.0;
    for (int i = 0; i < count; ++i)
        span_us = std::max(span_us, bar_end[static_cast<size_t>(i)]);
    // Left margin minimized: the concurrency rows carry no left labels (the old W0/W1
    // worker labels are gone), so the only thing in the gutter is the t=0 time-axis tick
    // label, centered at x = pad_l. 24px just clears its half-width ("0.0 µs" at 10px) with
    // a few px of breathing room, so the bars start as far left as the labels permit.
    const double pad_l = 24.0, pad_r = 28.0, plot_w = 1150.0;
    // Legend reflowed into three columns (3 rows, 18px pitch), height-optimised.
    const double header_h = 102.0, axis_h = 30.0, legend_h = 68.0;
    const double row_h = 30.0, bar_h = 20.0;
    // Ranking table below the legend: title + column header + one row per ranked node
    // + an omission footer when the share cutoff dropped any, 16px row pitch.
    const double table_row_h = 16.0;
    const double table_h = 42.0   // title + a one-line caption + a gap before the header row
        + table_row_h * static_cast<double>(1 + ranking.size() + (rank_omitted > 0 ? 1 : 0))
        + 12.0;
    const double px_per_us = plot_w / span_us;

    // A core-utilization area chart sits between the header and the rows: each time bucket
    // draws a bottom-anchored bar whose height is the utilization (full strip height = every
    // core busy), coloured on the same green->yellow->red ramp as the wash behind the bars.
    // Height carries the value, colour the verdict -- the shape stays readable where the
    // ramp's mid-tones alone would not discriminate. One row tall: enough vertical span for
    // the height signal without stealing plot space.
    const double util_strip_h = row_h;
    const double strip_gap = 6.0;
    // A view-bar of restyle buttons (script-wired) sits between the header text and the
    // util strip; the strip and everything below shift down by its height.
    const double btn_band_h = 30.0;
    const double strip_top = header_h + btn_band_h;
    const double rows_top = strip_top + util_strip_h + strip_gap;
    const double rows_bottom = rows_top + row_count * row_h;
    const double total_w = pad_l + plot_w + pad_r;
    const double total_h = rows_bottom + axis_h + legend_h + table_h;

    auto X = [&](double us) { return pad_l + us * px_per_us; };
    auto bar_top = [&](int i)
    {
        return rows_top + row_of[static_cast<size_t>(i)] * row_h + (row_h - bar_h) * 0.5;
    };

    // Per-edge critical dead-time band, classified by cause (see the gap_* constants):
    // the drawn gap between the predecessor's right edge and the successor's left edge,
    // joined with the time-bucketed utilization covering the same axis. The mean
    // utilization during the gap is overlap-WEIGHTED (a bucket contributes by the
    // fraction of the gap it covers -- partial buckets at the ends count partially);
    // min/max are unweighted over the touched buckets. Computed once here; consumed by
    // the headline split, the band rects, and the edge tooltips.
    struct Gap_info
    {
        bool has_band = false;
        double share = 0.0;
        double gap_us = 0.0;
        double t0 = 0.0, t1 = 0.0;   // gap window, µs from run start
        int cls = -1;                // 0 = dependency-bound, 1 = mixed, 2 = core-bound, -1 = no util data
        double mean_util = 0.0, min_util = 0.0, max_util = 0.0;
    };
    std::vector<Gap_info> gaps(edges_.size());
    for (size_t ei = 0; ei < edges_.size(); ++ei)
    {
        const Edge_agg& e = edges_[ei];
        Gap_info& g = gaps[ei];
        g.share = runs_ > 0
            ? static_cast<double>(e.critical_runs) / static_cast<double>(runs_) : 0.0;
        if (g.share < 0.10)
            continue;
        // Same geometry as the drawn band: predecessor right edge honours the 3px width
        // floor, converted back to time so the accounting matches the picture.
        g.t0 = bar_start[static_cast<size_t>(e.from)]
            + std::max(3.0 / px_per_us, bar_end[static_cast<size_t>(e.from)]
                                        - bar_start[static_cast<size_t>(e.from)]);
        g.t1 = bar_start[static_cast<size_t>(e.to)];
        g.gap_us = g.t1 - g.t0;
        if (g.gap_us * px_per_us <= 2.0)
            continue;
        g.has_band = true;
        if (!util_buckets_.empty() && util_bucket_width_us_ > 0.0)
        {
            double wsum = 0.0, usum = 0.0;
            double umin = 1.0, umax = 0.0;
            size_t b0 = static_cast<size_t>(std::max(0.0, std::floor(g.t0 / util_bucket_width_us_)));
            for (size_t b = b0; b < util_buckets_.size(); ++b)
            {
                double bt0 = static_cast<double>(b) * util_bucket_width_us_;
                if (bt0 >= g.t1)
                    break;
                if (util_buckets_[b].n == 0)
                    continue;
                double overlap = std::min(g.t1, bt0 + util_bucket_width_us_) - std::max(g.t0, bt0);
                if (overlap <= 0.0)
                    continue;
                double u = std::clamp(util_buckets_[b].mean, 0.0, 1.0);
                wsum += overlap;
                usum += overlap * u;
                umin = std::min(umin, u);
                umax = std::max(umax, u);
            }
            if (wsum > 0.0)
            {
                g.mean_util = usum / wsum;
                g.min_util = umin;
                g.max_util = umax;
                g.cls = g.mean_util >= gap_core_bound_share ? 2
                      : g.mean_util < gap_dependency_bound_share ? 0 : 1;
            }
        }
    }

    std::string out;
    char buf[512];
    auto line = [&](const char* fmt, auto... args)
    {
        std::snprintf(buf, sizeof buf, fmt, args...);
        out += buf;
    };

    // Render 20% larger by default: the drawing coordinates (viewBox) are unchanged, only
    // the presented width/height scale up, so text and geometry grow together with no
    // clipping risk.
    const double view_scale = 1.2;
    line("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.0f\" height=\"%.0f\" "
         "viewBox=\"0 0 %.0f %.0f\" font-family=\"'Segoe UI', sans-serif\">\n",
         total_w * view_scale, total_h * view_scale, total_w, total_h);
    line("<rect width=\"%.0f\" height=\"%.0f\" fill=\"#272822\"/>\n", total_w, total_h);
    // Hatch for the critical dead-time bands (and the legend swatch). The tile carries a
    // faint solid tint UNDER the diagonal stroke: a full-height band must read as "the
    // frame is losing time in this vertical slice" at a glance on #272822 -- hatch lines
    // alone wash out behind the bars crossing the band.
    // Three variants, one per dead-time cause: pink = dependency-bound (matches the
    // critical-pink chain semantics), amber = core-bound (capacity, distinct from both
    // critical pink and priority-H red), alternating pink/amber = mixed. An unclassified
    // band (no utilization data) draws the pink pattern.
    out += "<defs><pattern id=\"dead\" width=\"6\" height=\"6\" patternUnits=\"userSpaceOnUse\">"
           "<rect width=\"6\" height=\"6\" fill=\"#f92672\" opacity=\"0.10\"/>"
           "<path d=\"M0 6 L6 0\" stroke=\"#f92672\" stroke-width=\"1.4\" opacity=\"0.7\"/>"
           "</pattern>"
           "<pattern id=\"deadcore\" width=\"6\" height=\"6\" patternUnits=\"userSpaceOnUse\">"
           "<rect width=\"6\" height=\"6\" fill=\"#fd971f\" opacity=\"0.10\"/>"
           "<path d=\"M0 6 L6 0\" stroke=\"#fd971f\" stroke-width=\"1.4\" opacity=\"0.7\"/>"
           "</pattern>"
           "<pattern id=\"deadmix\" width=\"12\" height=\"6\" patternUnits=\"userSpaceOnUse\">"
           "<rect width=\"12\" height=\"6\" fill=\"#f92672\" opacity=\"0.05\"/>"
           "<rect width=\"12\" height=\"6\" fill=\"#fd971f\" opacity=\"0.05\"/>"
           "<path d=\"M0 6 L6 0\" stroke=\"#f92672\" stroke-width=\"1.4\" opacity=\"0.7\"/>"
           "<path d=\"M6 6 L12 0\" stroke=\"#fd971f\" stroke-width=\"1.4\" opacity=\"0.7\"/>"
           "</pattern></defs>\n";
    // Glossary affordance: header terms whose names may be unfamiliar carry the help cursor and a
    // styled overlay tooltip (the SAME scripted tooltip the graph nodes use -- multi-line, with
    // individually-coloured words -- reached by tagging each term `.hv` with a `data-tip`, not a
    // native <title>). The header text flows naturally (no manual layout); the overlay script draws
    // a rounded pill behind each `.term` at load time (getBBox), so the pills resolve wherever the
    // script runs (browser tab, <object>, <iframe>) -- inert under <img>, text intact.
    out += "<style>.term{cursor:help;}</style>\n";

    // Header: title, the dead-time headline, global stats. (The legend sits at the
    // bottom of the picture.)
    out += "<text x=\"16\" y=\"26\" font-size=\"15\" font-weight=\"600\" fill=\"#f8f8f2\">";
    std::string heading = title_.empty() ? "average run" : title_ + ": average run";
    if (runs_ > 0)
        heading += " \xE2\x80\x94 " + fmt_ms(makespan_.mean) + " ms";   // em dash + avg frame time
    append_escaped(out, heading);
    out += "</text>\n";
    {
        // The two frame classifiers on one coloured headline (band constants at the top of
        // this header): core utilization -- how much of the machine the frame used -- then
        // critical path dead time (frame time minus the binding chain's work) -- whether
        // the chain itself had to wait.
        double util = core_util_.mean;
        const char* util_color = util >= core_util_good_share ? "#a6e22e"
                               : util >= core_util_ok_share ? "#e6db74" : "#ff5f45";
        double dead_us = std::max(0.0, makespan_.mean - critical_work_.mean);
        double dead_share = makespan_.mean > 0.0 ? dead_us / makespan_.mean : 0.0;
        const char* dead_color = dead_share < dead_time_ok_share ? "#a6e22e"
                               : dead_share <= dead_time_bad_share ? "#e6db74" : "#ff5f45";
        struct Seg { std::string text; const char* color; bool term; const char* tip; };
        // Emit a term's `data-tip` attribute value: the term name as the tooltip headline (line 0),
        // then the multi-line body. A literal '\n' in an attribute is normalized to a space by the
        // XML parser, so newlines are written as the `&#10;` character reference (it survives and
        // reaches the overlay script as '\n'); the other markup-significant characters are escaped.
        // Backticks pass through untouched -- they delimit the overlay's coloured-run syntax.
        auto append_term_tip = [&](std::string_view name, const char* body)
        {
            append_escaped(out, name);
            for (const char* p = body; *p; ++p)
            {
                switch (*p)
                {
                case '\n': out += "&#10;"; break;
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                default: out.push_back(*p);
                }
            }
        };
        // Render one header line as a single <text> with flowing <tspan>s -- the browser lays it
        // out, so there is no manual x-cursor, no textLength, and no glyph distortion. xml:space
        // keeps the "  |  " separators intact. Term tspans carry `class="term hv"` + a `data-tip`
        // (the same scripted tooltip the nodes use, `data-hl` = the term's own colour for the
        // headline + border); the overlay script draws their pills at load time via getBBox.
        auto render_line = [&](double y, double F, const char* weight, const std::vector<Seg>& segs)
        {
            char buf[128];
            std::snprintf(buf, sizeof buf, "<text x=\"16\" y=\"%.0f\" font-size=\"%.0f\"%s xml:space=\"preserve\">",
                y, F, weight ? " font-weight=\"600\"" : "");
            out += buf;
            for (const Seg& s : segs)
            {
                out += "<tspan fill=\"";
                out += s.color;
                if (s.term)
                {
                    out += "\" class=\"term hv\" data-hl=\"";
                    out += s.color;
                    out += "\" data-tip=\"";
                    append_term_tip(s.text, s.tip);   // headline (name) + "\n" body; body starts "\n..."
                    out += "\">";
                }
                else
                {
                    out += "\">";
                }
                append_escaped(out, s.text);
                out += "</tspan>";
            }
            out += "</text>\n";
        };

        // Glossary tooltips: the term name is the headline (added by `append_term_tip`); each body
        // below starts with '\n' and is reflowed into short lines. Any ": " lands at a line END so
        // the overlay's field-bold heuristic does not misfire on prose (a deliberate leading
        // "Fix:" is the one exception -- bolding it reads as an intended label). The three banded
        // terms append a blank line then coloured green/yellow/red threshold rows (`RRGGBBtext`).
        static constexpr const char* TIP_UTIL =
            "\nShare of the machine's time that ran tasks."
            "\nLow means CPU cores sat idle."
            "\nCovers only configured worker threads."
            "\n"
            "\n`a6e22egreen`: >=75%"
            "\n`e6db74yellow`: 50-75%"
            "\n`ff5f45red`: <50%";
        static constexpr const char* TIP_DEAD =
            "\nFrame time minus the work on the longest"
            "\ndependency chain (critical path):"
            "\nhow long that chain sat waiting instead"
            "\nof working. High is an optimisation"
            "\nopportunity; the fix depends on the cause"
            "\n(see the split below):"
            "\ncore-bound - add cores if possible,"
            "\nor cut total work;"
            "\ndependency-bound - loosen dependencies or"
            "\nrearrange nodes, often profiler-guided."
            "\n"
            "\n`a6e22egreen`: <5%"
            "\n`e6db74yellow`: 5-10%"
            "\n`ff5f45red`: >10%";
        static constexpr const char* TIP_CORETIME =
            "\nTotal worker-time available in a frame:"
            "\nworkers x frame time. 100% the split to"
            "\nthe right divides up.";
        static constexpr const char* TIP_BODY =
            "\nCore-time spent running your task functors"
            "\n(the actual work), summed over all workers.";
        static constexpr const char* TIP_FO =
            "\nCore-time the framework spent for internal processing:"
            "\ngraph execution, launching tasks, scheduling"
            "\netc. High means tasks are too fine-grained."
            "\nTry reducing the number of tasks by moving"
            "\nparallelisation to the higher abstraction"
            "\nlayer (graph nodes)."
            "\n"
            "\n`a6e22egreen`: <=5%"
            "\n`e6db74yellow`: 5-15%"
            "\n`ff5f45red`: >15%";
        static constexpr const char* TIP_IDLE =
            "\nCore-time with no task to run (workers"
            "\nparked or spinning). High means the frame"
            "\ncan't keep the cores fed. Try splitting"
            "\nlong-running nodes or add internal node"
            "\nparallelisation.";
        static constexpr const char* TIP_FRAMETIME =
            "\nWall-clock length of the frame:"
            "\nfirst task starting to last task finishing.";
        static constexpr const char* TIP_CRITPATH =
            "\nThe fastest the frame could finish given"
            "\nonly task durations and dependencies, with"
            "\ninfinite amount of workers and CPU cores."
            "\nThe lower limit for frame time. Can only be"
            "\nimproved by loosening dependencies or"
            "\noptimising graph nodes internally.";
        static constexpr const char* TIP_BODY_TOTAL =
            "\nTotal work in the frame:"
            "\nevery task functor's time added up across"
            "\nall workers. Divide by workers for the"
            "\nperfect-parallel floor.";
        static constexpr const char* TIP_ORCH =
            "\nTime the calling thread spends building the"
            "\nrun each frame (binding links, re-arming"
            "\nnodes), off the worker cores.";

        // Row 2: core utilization + critical path dead time (+ its cause split). The cause split
        // apportions the globally-computed dead time across causes by the share-weighted gap
        // durations of the classified bands; a class with zero weight is omitted.
        {
            std::vector<Seg> segs;
            segs.push_back({ "core utilization", util_color, true, TIP_UTIL });
            segs.push_back({ ": " + fmt_us(100.0 * util) + "%", util_color, false, nullptr });
            segs.push_back({ "  |  ", "#cfcfc2", false, nullptr });
            segs.push_back({ "critical path dead time", dead_color, true, TIP_DEAD });
            segs.push_back({ ": " + fmt_us(dead_us) + " \xC2\xB5s (" + fmt_us(100.0 * dead_share) + "% of frame time)", dead_color, false, nullptr });

            double w_cls[3] = { 0.0, 0.0, 0.0 };
            double w_all = 0.0;
            for (const Gap_info& g : gaps)
                if (g.has_band && g.cls >= 0)
                {
                    double w = g.share * g.gap_us;
                    w_cls[g.cls] += w;
                    w_all += w;
                }
            if (w_all > 0.0 && dead_share > 0.0)
            {
                static constexpr const char* cls_color[3] = { "#f92672", "#e6db74", "#fd971f" };
                static constexpr const char* cls_word[3] = { "dependency", "mixed", "core-bound" };
                static constexpr const char* cls_tip[3] = {
                    "\nDead time where the chain waited on an"
                    "\nunfinished predecessor, not a busy core."
                    "\nFix: rearrange nodes, profiler-guided"
                    "\noptimisation or shorten the chain.",
                    "\nA stretch of dead time with both causes present:"
                    "\nsome cores free, some work still blocked.",
                    "\nDead time where the chain waited for a free"
                    "\ncore (every core was busy)."
                    "\nFix: add workers or shorten the chain."
                };
                segs.push_back({ " (", "#cfcfc2", false, nullptr });
                bool first_cls = true;
                for (int c : { 2, 1, 0 })   // core-bound first: the actionable surprise
                {
                    if (w_cls[c] > 0.0)
                    {
                        if (!first_cls)
                            segs.push_back({ " / ", "#cfcfc2", false, nullptr });
                        first_cls = false;
                        segs.push_back({ cls_word[c], cls_color[c], true, cls_tip[c] });
                        segs.push_back({ " " + fmt_us(100.0 * dead_share * w_cls[c] / w_all) + "%", cls_color[c], false, nullptr });
                    }
                }
                segs.push_back({ ")", "#cfcfc2", false, nullptr });
            }
            render_line(48.0, 12.0, "600", segs);
        }

        // Row 3: the core-time split -- body / framework overhead / idle (sums to 100%).
        {
            double fo = four_way_machinery_share();
            const char* fo_color = fo <= overhead_ok_share ? "#a6e22e"
                                 : fo <= overhead_bad_share ? "#e6db74" : "#ff5f45";
            std::vector<Seg> segs;
            segs.push_back({ "core-time", "#cfcfc2", true, TIP_CORETIME });
            segs.push_back({ ": ", "#cfcfc2", false, nullptr });
            segs.push_back({ "body", "#cfcfc2", true, TIP_BODY });
            segs.push_back({ " " + fmt_us(100.0 * four_way_body_share()) + "%  |  ", "#cfcfc2", false, nullptr });
            segs.push_back({ "framework overhead", fo_color, true, TIP_FO });
            segs.push_back({ " " + fmt_us(100.0 * fo) + "%", fo_color, false, nullptr });
            segs.push_back({ "  |  ", "#cfcfc2", false, nullptr });
            segs.push_back({ "idle", "#cfcfc2", true, TIP_IDLE });
            segs.push_back({ " " + fmt_us(100.0 * four_way_idle_share()) + "%", "#cfcfc2", false, nullptr });
            render_line(68.0, 12.0, "600", segs);
        }

        // Row 4: raw per-run stats. `body` here is the TOTAL body time (summed across cores);
        // `critical path` is the CPM lower bound; `orchestration` the off-core per-run setup.
        {
            std::vector<Seg> segs;
            segs.push_back({ "runs: " + std::to_string(runs_) + "  |  ", "#cfcfc2", false, nullptr });
            segs.push_back({ "frame time", "#cfcfc2", true, TIP_FRAMETIME });
            segs.push_back({ " " + fmt_ms(makespan_.mean) + " ms (min " + fmt_ms(makespan_min_)
                + " / max " + fmt_ms(makespan_max_) + ")  |  ", "#cfcfc2", false, nullptr });
            segs.push_back({ "critical path", "#cfcfc2", true, TIP_CRITPATH });
            segs.push_back({ " " + fmt_ms(cpm_us) + " ms  |  workers: " + std::to_string(workers_seen)
                + "  |  tasks ~" + std::to_string(static_cast<long long>(std::llround(tasks_per_run_.mean)))
                + "/run  |  per run: ", "#cfcfc2", false, nullptr });
            segs.push_back({ "body", "#cfcfc2", true, TIP_BODY_TOTAL });
            segs.push_back({ " " + fmt_ms(body_us_.mean) + " ms / framework " + fmt_ms(machinery_us_.mean) + " ms / ", "#cfcfc2", false, nullptr });
            segs.push_back({ "orchestration", "#cfcfc2", true, TIP_ORCH });
            segs.push_back({ " " + fmt_us(four_way_orchestration_us()) + " \xC2\xB5s", "#cfcfc2", false, nullptr });
            render_line(86.0, 11.0, nullptr, segs);
        }
    }

    // View bar: a row of restyle buttons below the header. Each is a `<g class="btn"
    // data-view="...">` with a rounded rect + centered label; the overlay script wires
    // clicks (toggle or cycle a view, brighten the active button, relabel the cycles).
    // Widths are sized for the widest cycle label so relabeling never overflows the rect.
    // Inert under <img> (script-gated) like the rest of the interactivity.
    {
        struct Btn { const char* view; const char* label; double w; };
        static constexpr Btn btns[] = {
            { "crit",   "Critical focus",  100.0 },
            { "edges",  "Edges: on",        78.0 },
            { "kind",   "Edge type: all",  116.0 },
            { "cone",   "Cone: off",        82.0 },
            { "dead",   "Dead-time focus", 112.0 },
            { "metric", "Colour: crit",    104.0 },
        };
        double bx = 16.0;
        const double by = header_h + 5.0;
        const double bh = 20.0;
        for (const Btn& b : btns)
        {
            out += "<g class=\"btn\" data-view=\"";
            out += b.view;
            out += "\" style=\"cursor:pointer\">";
            line("<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" rx=\"4\" fill=\"#3e3d32\" "
                 "stroke=\"#75715e\" stroke-width=\"1\"/>", bx, by, b.w, bh);
            line("<text x=\"%.1f\" y=\"%.1f\" font-size=\"11\" fill=\"#cfcfc2\" text-anchor=\"middle\">",
                 bx + b.w * 0.5, by + 14.0);
            append_escaped(out, b.label);
            out += "</text></g>\n";
            bx += b.w + 8.0;
        }
        line("<text x=\"%.1f\" y=\"%.1f\" font-size=\"10\" fill=\"#75715e\">views: click to toggle; "
             "cycles relabel; Cone: pick a node</text>\n", bx + 6.0, by + 14.0);
    }

    // Time grid + axis labels: a 1/2/5-series step giving at most ~8 ticks.
    {
        double step = std::pow(10.0, std::floor(std::log10(std::max(1.0, span_us / 8.0))));
        for (double mult : { 1.0, 2.0, 5.0, 10.0, 20.0, 50.0 })
            if (span_us / (step * mult) <= 8.0)
            {
                step *= mult;
                break;
            }
        for (double t = 0.0; t <= span_us + 1e-9; t += step)
        {
            line("<line x1=\"%.1f\" y1=\"%.0f\" x2=\"%.1f\" y2=\"%.0f\" stroke=\"#3e3d32\" stroke-width=\"1\"/>\n",
                 X(t), strip_top - 4.0, X(t), rows_bottom);
            std::string lbl = fmt_us(t) + " \xC2\xB5s";
            out += "<text x=\"" + std::to_string(X(t)) + "\" y=\"" + std::to_string(rows_bottom + 18.0)
                 + "\" font-size=\"10\" fill=\"#cfcfc2\" text-anchor=\"middle\">";
            append_escaped(out, lbl);
            out += "</text>\n";
        }
    }

    {
        // Legend, at the bottom (below the time axis), in THREE columns of up to three
        // rows (height-optimised): a short sample glyph, then its explanation, inline.
        // Line STYLE carries provenance (solid = explicit, dashed = derived); colour
        // carries criticality (green -> pink by share of binding chains). Column 1 =
        // edges, column 2 = critical node + dead-time causes, column 3 = priorities,
        // welds. The utilization key lives above the strip itself, not here.
        const double cols[3] = { 16.0, 430.0, 820.0 };   // per-column glyph x; 40px glyph span
        const double glyph_w = 40.0;
        const double base = rows_bottom + axis_h;
        auto legend_arrow = [&](double cx, double ly, const char* stroke, const char* dash, const char* text)
        {
            line("<line x1=\"%.0f\" y1=\"%.0f\" x2=\"%.0f\" y2=\"%.0f\" stroke=\"%s\" stroke-width=\"1.8\"%s/>\n",
                 cx, ly, cx + glyph_w, ly, stroke, dash);
            line("<polygon points=\"%.0f,%.0f %.0f,%.0f %.0f,%.0f\" fill=\"%s\"/>\n",
                 cx + glyph_w, ly - 4.0, cx + glyph_w, ly + 4.0, cx + glyph_w + 7.0, ly, stroke);
            line("<text x=\"%.0f\" y=\"%.0f\" font-size=\"11\" fill=\"#cfcfc2\">%s</text>\n",
                 cx + glyph_w + 14.0, ly + 4.0, text);
        };
        auto legend_swatch = [&](double cx, double ly, const char* fill, const char* extra, const char* text)
        {
            line("<rect x=\"%.0f\" y=\"%.0f\" width=\"%.0f\" height=\"10\" rx=\"2\" fill=\"%s\"%s/>\n",
                 cx, ly - 5.0, glyph_w, fill, extra);
            line("<text x=\"%.0f\" y=\"%.0f\" font-size=\"11\" fill=\"#cfcfc2\">%s</text>\n",
                 cx + glyph_w + 14.0, ly + 4.0, text);
        };
        legend_arrow(cols[0], base + 14.0, "#a6e22e", "", "explicit ordering (after/before)");
        legend_arrow(cols[0], base + 32.0, "#a6e22e", " stroke-dasharray=\"5,3\"", "derived from declared access");
        legend_arrow(cols[0], base + 50.0, "#f92672", "", "critical-path edge (pink = share of runs)");
        legend_swatch(cols[1], base + 14.0, "#3e3d32", " stroke=\"#fd971f\" stroke-width=\"2\"",
                      "critical node (orange = share of runs)");
        legend_swatch(cols[1], base + 32.0, "url(#dead)", "", "critical dead time, dependency-bound");
        legend_swatch(cols[1], base + 50.0, "url(#deadcore)", "", "critical dead time, core-bound: cores busy");
        line("<text x=\"%.0f\" y=\"%.0f\" font-size=\"11\">"
             "<tspan fill=\"%s\">high</tspan><tspan fill=\"#cfcfc2\"> / </tspan>"
             "<tspan fill=\"%s\">normal</tspan><tspan fill=\"#cfcfc2\"> / </tspan>"
             "<tspan fill=\"%s\">low</tspan>"
             "<tspan fill=\"#cfcfc2\"> = node name colour = priority</tspan></text>\n",
             cols[2], base + 18.0,
             priority_color(Priority::high), priority_color(Priority::normal), priority_color(Priority::low));
        const double lyw = base + 32.0;
        line("<circle cx=\"%.0f\" cy=\"%.0f\" r=\"5\" fill=\"#a6e22e\"/>\n", cols[2] + glyph_w * 0.5, lyw);
        line("<text x=\"%.0f\" y=\"%.0f\" font-size=\"11\" fill=\"#cfcfc2\">handoff weld: nodes back to back</text>\n",
             cols[2] + glyph_w + 14.0, lyw + 4.0);
    }

    // Critical-path ranking table, below the legend: the picture's criticality colouring
    // as a sorted list -- rank key = chain share x mean duration (expected contribution
    // to the binding chain), so the heavy usually-critical nodes top it. Monospace, with
    // per-column x anchors (right-aligned numerics) rather than space padding.
    {
        const double tbase = rows_bottom + axis_h + legend_h;
        const char* mono = "Consolas, 'Cascadia Mono', monospace";
        // Column right edges (numeric, text-anchor=end) / left edges (rank gets end too).
        const double cx_rank = 34.0, cx_name = 46.0, cx_share = 268.0, cx_mean = 356.0,
                     cx_key = 452.0, cx_dead = 548.0, cx_slack = 628.0, cx_p95 = 712.0;
        std::string title_row = "critical-path ranking \xE2\x80\x94 "
            + std::to_string(runs_) + " runs, frame time " + fmt_ms(makespan_.mean) + " ms";
        out += "<text x=\"16\" y=\"" + std::to_string(tbase + 16.0)
             + "\" font-size=\"12\" font-weight=\"600\" fill=\"#f8f8f2\">";
        append_escaped(out, title_row);
        out += "</text>\n";
        // One-line caption: what the table is.
        out += "<text x=\"16\" y=\"" + std::to_string(tbase + 32.0) + "\" font-size=\"11\" fill=\"#cfcfc2\">";
        append_escaped(out, "Nodes ranked by expected contribution to the binding critical chain "
                            "(mean \xC3\x97 chain%) \xE2\x80\x94 where optimising the frame helps most.");
        out += "</text>\n";
        double ty = tbase + 42.0 + table_row_h;
        auto cell = [&](double cx, const char* anchor, const std::string& fill, const std::string& text)
        {
            out += "<text x=\"" + std::to_string(cx) + "\" y=\"" + std::to_string(ty)
                 + "\" font-size=\"11\" font-family=\"" + std::string(mono) + "\" text-anchor=\""
                 + anchor + "\" fill=\"" + fill + "\">";
            append_escaped(out, text);
            out += "</text>\n";
        };
        // A column header styled exactly like the header glossary terms: a `term hv` tspan gets a
        // pill (drawn by the overlay's getBBox loop, which targets a tspan inside a text) + a
        // styled hover tooltip, so the affordance is visible. `#`/`node` stay plain.
        auto hcell = [&](double cx, const char* anchor, const std::string& text, std::initializer_list<const char*> tip)
        {
            out += "<text x=\"" + std::to_string(cx) + "\" y=\"" + std::to_string(ty)
                 + "\" font-size=\"11\" font-family=\"" + std::string(mono) + "\" text-anchor=\""
                 + anchor + "\" fill=\"#cfcfc2\"><tspan class=\"term hv\" data-hl=\"#66d9ef\" data-tip=\"";
            bool first = true;
            for (const char* ln : tip)
            {
                if (!first)
                    out += "&#10;";
                first = false;
                append_escaped(out, ln);
            }
            out += "\">";
            append_escaped(out, text);
            out += "</tspan></text>\n";
        };
        cell(cx_rank, "end", "#75715e", "#");
        cell(cx_name, "start", "#75715e", "node");
        hcell(cx_share, "end", "chain%", { "chain %",
            "How often this node was on the frame's binding",
            "critical chain, across the runs.",
            "High = it usually decides frame time." });
        hcell(cx_mean, "end", "mean", { "mean",
            "Mean wall-clock duration of this node's bar",
            "over the runs." });
        hcell(cx_key, "end", "x share", { "mean \xC3\x97 chain%",
            "The ranking key: mean duration times chain share",
            "= this node's expected contribution to frame time.",
            "The table sorts by this." });
        hcell(cx_dead, "end", "dead-in", { "dead-in",
            "Incoming critical dead time: how long this node sat",
            "ready but waiting (for a core or a predecessor)",
            "before it ran, on runs where it bound the chain." });
        hcell(cx_slack, "end", "slack", { "slack",
            "How far this node could slip without extending the frame",
            "(structural float from the critical-path method).",
            "0 = on the critical path, with no room to slip." });
        hcell(cx_p95, "end", "P95", { "P95",
            "95th-percentile duration: the node's near-worst-case",
            "time, vs its mean." });
        const std::string us = " \xC2\xB5s";
        for (size_t r = 0; r < ranking.size(); ++r)
        {
            const Rank_row& row = ranking[r];
            const Node_agg& a = nodes_[static_cast<size_t>(row.node)];
            ty += table_row_h;
            // chain% takes the same cyan->orange criticality blend as the node borders.
            std::string share_col = blend_hex(0x66, 0xd9, 0xef, 0xfd, 0x97, 0x1f, crit_ramp(row.share));
            char pct[16];
            std::snprintf(pct, sizeof pct, "%.0f%%", row.share * 100.0);
            cell(cx_rank, "end", "#cfcfc2", std::to_string(r + 1));
            cell(cx_name, "start", priority_color(a.priority), a.label);
            cell(cx_share, "end", share_col, pct);
            cell(cx_mean, "end", "#cfcfc2", fmt_us(a.duration.mean) + us);
            cell(cx_key, "end", "#cfcfc2", fmt_us(row.key) + us);
            cell(cx_dead, "end", "#cfcfc2", row.dead_in < 0.0 ? "-" : fmt_us(row.dead_in) + us);
            cell(cx_slack, "end", "#cfcfc2",
                 slack[static_cast<size_t>(row.node)] <= 0.5 ? "0"
                     : fmt_us(slack[static_cast<size_t>(row.node)]) + us);
            cell(cx_p95, "end", "#cfcfc2", fmt_us(a.dur_P95.value()) + us);
        }
        if (rank_omitted > 0)
        {
            ty += table_row_h;
            char pct[16];
            std::snprintf(pct, sizeof pct, "%.0f%%", rank_max_omitted_share * 100.0);
            char cutoff[16];
            std::snprintf(cutoff, sizeof cutoff, "%.0f%%", rank_min_share * 100.0);
            std::string foot = std::to_string(rank_omitted)
                + (rank_omitted == 1 ? " node" : " nodes")
                + " with chain share < " + cutoff + " omitted";
            if (rank_max_omitted >= 0 && rank_max_omitted_share > 0.0)
                foot += " (max: " + nodes_[static_cast<size_t>(rank_max_omitted)].label + " " + pct + ")";
            cell(cx_name, "start", "#75715e", foot);
        }
    }

    // No row separators or labels: rows are anonymous concurrency slots, not workers --
    // there is nothing true to label them with.

    // Multi-line tooltip data: each line escaped, joined with a literal `&#10;` character
    // reference -- a raw newline in an attribute value would be normalized to a space by
    // the XML parser; the reference survives and reaches the overlay script as '\n'.
    auto append_tip_attr = [&](const std::vector<std::string>& lines_v)
    {
        for (size_t k = 0; k < lines_v.size(); ++k)
        {
            if (k > 0)
                out += "&#10;";
            append_escaped(out, lines_v[k]);
        }
    };

    // Core-utilization background: a faint full-height wash coloured by the TRUE per-time core
    // utilization -- green (all cores busy) -> yellow (half) -> red (idle). Sampled from
    // per-worker busy time bucketed over the run (the scheduler's per-bucket counters via
    // trace_stamps), so every task kind counts at its real time -- a `parallel_for` node's
    // fan-out across cores registers fully, which the old node-concurrency proxy undercounted.
    // Green stretches saturate the cores; red valleys are idle capacity (where deferred work
    // could go). Drawn first: behind the dead-time bands and bars, inert to hover.
    if (!util_buckets_.empty() && util_bucket_width_us_ > 0.0)
    {
        const double x_right = pad_l + plot_w;
        for (size_t b = 0; b < util_buckets_.size(); ++b)
        {
            if (util_buckets_[b].n == 0)
                continue;
            double u = std::clamp(util_buckets_[b].mean, 0.0, 1.0);
            double x0 = X(static_cast<double>(b) * util_bucket_width_us_);
            double x1 = X(static_cast<double>(b + 1) * util_bucket_width_us_);
            if (x0 >= x_right)
                break;
            if (x1 > x_right)
                x1 = x_right;
            // green (busy) -> yellow (half) -> red (idle), the utilization-headline ramp.
            std::string c = u >= 0.5
                ? blend_hex(0xe6, 0xdb, 0x74, 0xa6, 0xe2, 0x2e, (u - 0.5) * 2.0)
                : blend_hex(0xff, 0x5f, 0x45, 0xe6, 0xdb, 0x74, u * 2.0);
            line("<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" fill=\"%s\" "
                 "opacity=\"0.24\" pointer-events=\"none\"/>\n",
                 x0, rows_top, x1 - x0, rows_bottom - rows_top, c.c_str());
        }

        // The utilization area chart above the rows: per bucket, a bottom-anchored bar of
        // height u x strip height in the bucket's ramp colour at full opacity -- an area
        // chart in bucket-wide steps. Faint frame lines mark 0% (baseline) and 100% (top)
        // so the height reads against a scale. Each bucket is hoverable over the full strip
        // height (`.hv`, via a transparent hit rect) -- the tooltip reports the exact
        // core-utilization % and time at the cursor.
        // The 100% line is red: unreached headroom between the bars and the line is the
        // idle-capacity signal, and the contrast makes the shortfall legible at a glance.
        line("<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#ff5f45\" "
             "stroke-width=\"0.8\" opacity=\"0.8\"/>\n",
             pad_l, strip_top, x_right, strip_top);
        // The strip's key sits right above it (not in the bottom legend): a small
        // green->yellow->red gradient swatch + label, right-aligned to the plot edge.
        out += "<defs><linearGradient id=\"utilkey\">"
               "<stop offset=\"0\" stop-color=\"#a6e22e\"/><stop offset=\"0.5\" stop-color=\"#e6db74\"/>"
               "<stop offset=\"1\" stop-color=\"#ff5f45\"/></linearGradient></defs>\n";
        line("<rect x=\"%.1f\" y=\"%.1f\" width=\"40\" height=\"8\" rx=\"2\" fill=\"url(#utilkey)\" opacity=\"0.8\"/>\n",
             x_right - 40.0, strip_top - 13.0);
        line("<text x=\"%.1f\" y=\"%.1f\" font-size=\"11\" fill=\"#cfcfc2\" text-anchor=\"end\">"
             "core utilization over time</text>\n",
             x_right - 46.0, strip_top - 5.0);
        line("<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#75715e\" "
             "stroke-width=\"0.6\" opacity=\"0.8\"/>\n",
             pad_l, strip_top + util_strip_h, x_right, strip_top + util_strip_h);
        for (size_t b = 0; b < util_buckets_.size(); ++b)
        {
            if (util_buckets_[b].n == 0)
                continue;
            double u = std::clamp(util_buckets_[b].mean, 0.0, 1.0);
            double x0 = X(static_cast<double>(b) * util_bucket_width_us_);
            double x1 = X(static_cast<double>(b + 1) * util_bucket_width_us_);
            if (x0 >= x_right)
                break;
            if (x1 > x_right)
                x1 = x_right;
            std::string c = u >= 0.5
                ? blend_hex(0xe6, 0xdb, 0x74, 0xa6, 0xe2, 0x2e, (u - 0.5) * 2.0)
                : blend_hex(0xff, 0x5f, 0x45, 0xe6, 0xdb, 0x74, u * 2.0);
            double t_ms = (static_cast<double>(b) + 0.5) * util_bucket_width_us_ / 1000.0;
            double bar_hgt = u * util_strip_h;
            char tip[96];
            std::snprintf(tip, sizeof tip, "core utilization: %.0f%%&#10;t %.2f ms", u * 100.0, t_ms);
            out += "<g class=\"hv\" data-hl=\"" + c + "\" data-tip=\"" + tip + "\">";
            line("<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" fill=\"#000\" "
                 "fill-opacity=\"0\" pointer-events=\"all\"/>\n",
                 x0, strip_top, x1 - x0, util_strip_h);
            line("<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" fill=\"%s\"/>\n",
                 x0, strip_top + util_strip_h - bar_hgt, x1 - x0, bar_hgt, c.c_str());
            out += "</g>\n";
        }
    }

    // Critical dead-time regions, in the BACKGROUND and FULL HEIGHT: the wait is a
    // property of the binding chain, not of any row or worker. For an edge that binds
    // >= 10% of runs, the region is the VISIBLE gap on screen between the predecessor's
    // drawn right edge and the successor's drawn left edge -- pure screen coordinates, so
    // it is always the real break between two bars and can never land on a bar (the old
    // form subtracted a MEAN gap from a MEDIAN bar start, mixing coordinate systems).
    // Each region's OPACITY scales with its edge's criticality share, so a wait that binds
    // rarely (say 11%) draws faint and a wait that binds most runs draws strong -- otherwise
    // an occasional-minority wait, drawn at full strength, paints over the strongly-critical
    // spine running on another row in the same window and reads as if the real critical path
    // were dead time. Drawn PER EDGE (not unioned): unioning would let one faint region
    // inherit an overlapping strong sliver's share and defeat the weighting; overlaps just
    // stack, bounded since low-share bands are faint. These localize the headline critical
    // dead time (frame_time_mean - critical_work_mean, computed globally) -- their >= 10%-share
    // visible subset, weighted by frequency, not a sum. Behind the bars, `pointer-events: none`.
    // The pattern carries the band's CAUSE (see `Gap_info`): pink hatch = dependency-bound,
    // amber = core-bound, alternating = mixed; unclassified (no utilization data) stays
    // pink. Bands remain inert to hover (`pointer-events: none`) -- making them hoverable
    // would fight the bars and edges drawn over them; the classification detail lives in
    // the edge's tooltip instead.
    for (size_t ei = 0; ei < edges_.size(); ++ei)
    {
        const Gap_info& g = gaps[ei];
        if (!g.has_band)
            continue;
        const char* pattern = g.cls == 2 ? "deadcore" : g.cls == 1 ? "deadmix" : "dead";
        line("<rect class=\"deadband\" x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" "
             "fill=\"url(#%s)\" opacity=\"%.2f\" pointer-events=\"none\"/>\n",
             X(g.t0), rows_top, (g.t1 - g.t0) * px_per_us, rows_bottom - rows_top,
             pattern, std::clamp(g.share, 0.10, 1.0));
    }

    // Colored inline segments for the scripted tooltip: `RRGGBBtext` (backtick-delimited,
    // 6 hex digits then text) -- the overlay renders that run in that colour. Backtick
    // never appears in a label / resource / number, so it is a safe sentinel.
    auto seg = [](const char* hex6, std::string_view text)
    {
        std::string s = "`";
        s += hex6;
        s.append(text);
        s += "`";
        return s;
    };
    auto mode_seg = [&](bool write)   // RW red / RO green, matching the access colours
    {
        return seg(write ? "ff5f45" : "a6e22e", write ? "RW" : "RO");
    };
    auto crit_seg = [&](double share, const std::string& name)
    {
        std::string hex = blend_hex(0xa6, 0xe2, 0x2e, 0xf9, 0x26, 0x72, crit_ramp(share));
        return seg(hex.c_str() + 1, name);   // skip the '#'
    };

    // Incoming / outgoing edge index lists per node, for the tooltip edge lists.
    std::vector<std::vector<int>> node_in(static_cast<size_t>(count)), node_out(static_cast<size_t>(count));
    for (int ei = 0; ei < static_cast<int>(edges_.size()); ++ei)
    {
        node_out[static_cast<size_t>(edges_[static_cast<size_t>(ei)].from)].push_back(ei);
        node_in[static_cast<size_t>(edges_[static_cast<size_t>(ei)].to)].push_back(ei);
    }
    auto edge_share = [&](int ei)
    {
        return runs_ > 0
            ? static_cast<double>(edges_[static_cast<size_t>(ei)].critical_runs) / static_cast<double>(runs_) : 0.0;
    };
    // Raw numeric attribute value (no unit) for the view-bar's data-* metrics.
    auto attr_num = [](double v)
    {
        char b[32];
        std::snprintf(b, sizeof b, "%.4f", v);
        return std::string(b);
    };

    // Bars (tooltip data on the group; the overlay script renders it), then edges on top.
    for (int i = 0; i < count; ++i)
    {
        const Node_agg& a = nodes_[static_cast<size_t>(i)];
        Node_stats s = node_stats(i);
        double x = X(bar_start[static_cast<size_t>(i)]);
        // Floor the drawn width so near-zero-duration nodes (the publish flip, ~µs) stay
        // visible and hoverable; the truthful duration is in the tooltip.
        double w = std::max(3.0, (bar_end[static_cast<size_t>(i)] - bar_start[static_cast<size_t>(i)]) * px_per_us);
        double yb = bar_top(i);

        std::vector<std::string> tip;
        tip.push_back(a.label);   // line 0 headline; priority shown right-aligned (data-prio)
        tip.push_back("Exec: mean " + fmt_us(s.mean_us) + " | P95 " + fmt_us(s.P95_us)
             + " | \xCF\x83 " + fmt_us(s.stddev_us)
             + " (CV " + fmt_us(s.mean_us > 0.0 ? 100.0 * s.stddev_us / s.mean_us : 0.0) + "%)"
             + " | min/max " + fmt_us(s.min_us) + "/" + fmt_us(s.max_us) + " \xC2\xB5s");
        // True busy (body + slices + async fan-out): for a parallel_for node this exceeds the
        // bar duration -- the bar is wall time, this is core time across the fan-out.
        if (s.true_busy_us > 0.5)
            tip.push_back("True busy (body+slices+async): mean " + fmt_us(s.true_busy_us) + " \xC2\xB5s");
        // No worker line: workers are interchangeable, and rows carry no worker identity.
        tip.push_back("Critical: in " + fmt_us(100.0 * s.critical_share) + "% of runs");
        if (slack[static_cast<size_t>(i)] > 0.5)
            tip.push_back("Slack: " + fmt_us(slack[static_cast<size_t>(i)]) + " \xC2\xB5s");
        tip.push_back("Dispatch wait: mean " + fmt_us(s.dispatch_wait_us) + " \xC2\xB5s");
        if (!a.accesses.empty())
        {
            std::string acc = "Access: ";   // RO green / RW red per declared mode
            for (size_t k = 0; k < a.accesses.size(); ++k)
            {
                if (k > 0)
                    acc += "; ";
                acc += a.accesses[k].first + ": " + mode_seg(a.accesses[k].second);
            }
            tip.push_back(std::move(acc));
        }
        // Incoming / outgoing edges (only if any): the OTHER node's name, coloured by that
        // edge's share of binding chains (green -> pink), so the node's place in the
        // critical structure reads at a glance.
        if (!node_in[static_cast<size_t>(i)].empty())
        {
            std::string s2 = "->|  ";
            bool first = true;
            for (int ei : node_in[static_cast<size_t>(i)])
            {
                if (!first) s2 += ", ";
                first = false;
                s2 += crit_seg(edge_share(ei), nodes_[static_cast<size_t>(edges_[static_cast<size_t>(ei)].from)].label);
            }
            tip.push_back(std::move(s2));
        }
        if (!node_out[static_cast<size_t>(i)].empty())
        {
            std::string s2 = "|->  ";
            bool first = true;
            for (int ei : node_out[static_cast<size_t>(i)])
            {
                if (!first) s2 += ", ";
                first = false;
                s2 += crit_seg(edge_share(ei), nodes_[static_cast<size_t>(edges_[static_cast<size_t>(ei)].to)].label);
            }
            tip.push_back(std::move(s2));
        }

        // Two colour channels on a bar: the BORDER blends cyan -> orange with the node's
        // share of binding chains (`crit_ramp`, label weight follows -- no single-path
        // pretense; frequency IS the cross-run answer), while the NAME text is filled with
        // the priority colour (red high / green normal / brightened grey low).
        double hf = crit_ramp(s.critical_share);
        std::string color = blend_hex(0x66, 0xd9, 0xef, 0xfd, 0x97, 0x1f, hf);
        double border_w = 1.0 + 1.5 * hf;
        int weight = 400 + static_cast<int>(std::lround(300.0 * hf));
        const char* label_fill = priority_color(a.priority);

        // data-nc: tooltip headline (node name) in the priority colour. data-prio: the
        // right-aligned priority tag on that line, same colour.
        out += "<g class=\"hv\" data-node=\"" + std::to_string(i)
            + "\" data-crit=\"" + attr_num(s.critical_share)
            + "\" data-dispatch=\"" + attr_num(s.dispatch_wait_us)
            + "\" data-slack=\"" + attr_num(slack[static_cast<size_t>(i)])
            + "\" data-hl=\"" + color
            + "\" data-nc=\"" + label_fill
            + "\" data-prio=\"" + priority_word(a.priority) + "-pri\" data-pc=\"" + label_fill
            + "\" data-tip=\"";
        append_tip_attr(tip);
        out += "\">\n";
        line("<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.0f\" rx=\"3\" "
             "fill=\"#3e3d32\" stroke=\"%s\" stroke-width=\"%.1f\"/>\n",
             x, yb, w, bar_h, color.c_str(), border_w);
        double text_w = 6.4 * static_cast<double>(a.label.size());
        if (text_w + 8.0 <= w)
        {
            line("<text x=\"%.1f\" y=\"%.1f\" font-size=\"11\" font-weight=\"%d\" "
                 "fill=\"%s\" text-anchor=\"middle\">", x + w * 0.5, yb + 14.0, weight, label_fill);
            append_escaped(out, a.label);
            out += "</text>\n";
        }
        else
        {
            line("<text x=\"%.1f\" y=\"%.1f\" font-size=\"10\" font-weight=\"%d\" "
                 "fill=\"%s\">", x + w + 5.0, yb + 14.0, weight, label_fill);
            append_escaped(out, a.label);
            out += "</text>\n";
        }
        out += "</g>\n";
    }

    for (size_t edge_i = 0; edge_i < edges_.size(); ++edge_i)
    {
        const Edge_agg& e = edges_[edge_i];
        // Gantt-style anchors: exit at the source bar's right-edge MIDPOINT, enter at the
        // target's left-edge midpoint. The exit uses the drawn right edge (bars have a
        // 3px width floor), so an arrow never starts inside a collapsed bar.
        double x1 = X(bar_start[static_cast<size_t>(e.from)])
            + std::max(3.0, (bar_end[static_cast<size_t>(e.from)]
                             - bar_start[static_cast<size_t>(e.from)]) * px_per_us);
        double y1 = bar_top(e.from) + bar_h * 0.5;
        double x2 = X(bar_start[static_cast<size_t>(e.to)]);
        double y2 = bar_top(e.to) + bar_h * 0.5;
        // Horizontal tangent length: proportional to the travel, floored at 18px so a
        // meet-point-clamped edge (near-zero horizontal gap) still leaves and arrives
        // flat, capped so short edges don't balloon.
        double tangent = std::clamp(0.45 * std::abs(x2 - x1) + 0.15 * std::abs(y2 - y1), 18.0, 64.0);
        double share = runs_ > 0
            ? static_cast<double>(e.critical_runs) / static_cast<double>(runs_) : 0.0;
        std::vector<std::string> tip;
        tip.push_back(e.explicit_ordering ? "explicit ordering" : "derived from declared access");
        // Conflict detail (from graph_introspect: "resource: RW->RO; ...") rendered per
        // resource as a `"resource" access` line then a `{from} RW -> {to} RO` line, with
        // RO/RW colour-coded (green/red) and the node names in default light.
        if (!e.conflict.empty())
        {
            const std::string& fromName = nodes_[static_cast<size_t>(e.from)].label;
            const std::string& toName = nodes_[static_cast<size_t>(e.to)].label;
            size_t pos = 0;
            while (pos < e.conflict.size())
            {
                size_t semi = e.conflict.find("; ", pos);
                std::string piece = e.conflict.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
                pos = semi == std::string::npos ? e.conflict.size() : semi + 2;
                size_t colon = piece.find(": ");
                size_t arrow = piece.find("->");
                if (colon == std::string::npos || arrow == std::string::npos)
                    continue;
                std::string res = piece.substr(0, colon);
                std::string fm = piece.substr(colon + 2, arrow - (colon + 2));
                std::string tm = piece.substr(arrow + 2);
                tip.push_back("\"" + res + "\" access");
                tip.push_back(fromName + " " + mode_seg(fm == "RW") + " -> " + toName + " " + mode_seg(tm == "RW"));
            }
        }
        tip.push_back("Critical: in " + fmt_us(100.0 * share) + "% of runs");
        if (share >= 0.10 && e.binding_gap.mean > 0.5)
            tip.push_back("Critical wait: " + fmt_us(e.binding_gap.mean) + " \xC2\xB5s mean");
        // Dead-time block, when this edge has a drawn band: the gap, its cause (the
        // utilization-join classification, colour-matched to the band's hatch), the
        // utilization evidence, and the lever the cause implies.
        if (const Gap_info& g = gaps[edge_i]; g.has_band)
        {
            tip.push_back("Dead time: " + fmt_us(g.gap_us) + " \xC2\xB5s (drawn gap, "
                + fmt_us(100.0 * g.share) + "% of runs weighted)");
            if (g.cls >= 0)
            {
                static constexpr const char* gap_hex[3] = { "f92672", "e6db74", "fd971f" };
                static constexpr const char* gap_word[3] = { "dependency-bound", "mixed", "core-bound" };
                tip.push_back("Cause: " + seg(gap_hex[g.cls], gap_word[g.cls])
                    + " (cores " + fmt_us(100.0 * g.mean_util) + "% busy during gap, "
                    + fmt_us(100.0 * g.min_util) + "-" + fmt_us(100.0 * g.max_util) + "% across buckets)");
                tip.push_back(g.cls == 2
                    ? "Lever: add workers or move off-path work out of this window"
                    : g.cls == 0
                    ? "Lever: cut/loosen this edge or overlap the producer"
                    : "Lever: both apply -- part capacity, part dependency");
            }
            else
            {
                tip.push_back("Cause: unclassified (no utilization data for this window)");
            }
        }
        // Dash carries provenance (solid = explicit, dashed = derived); colour carries
        // criticality, green -> pink by the edge's share of binding chains. Width is
        // uniform per kind.
        double crit = crit_ramp(share);
        std::string stroke = blend_hex(0xa6, 0xe2, 0x2e, 0xf9, 0x26, 0x72, crit);
        const char* extra = e.explicit_ordering ? "" : " stroke-dasharray=\"5,3\"";
        double width = e.explicit_ordering ? 2.0 : 1.5;

        // Handoff weld: connected bars back to back on one row (the object-handoff
        // elision often runs a successor immediately on the settling worker) collapse
        // the curve to nothing -- draw a dot straddling the junction instead. Provenance
        // (solid vs dashed) is tooltip-only for welds; the dot keeps the criticality
        // colour, and marks exactly where the handoff fired.
        bool weld = std::abs(y2 - y1) < 0.5 && x2 - x1 < 5.0;
        if (weld)
            tip.push_back("Handoff: back-to-back with the predecessor");

        // Edges are ALL uniformly faint at rest so the picture reads as bars first --
        // including critical ones, which keep their pink colour but not extra opacity
        // (critical structure at rest is carried by the orange node borders/labels).
        // Hovering a node brightens its incident edges to full (see the overlay); nothing
        // is permanently bright. data-a/data-b are the endpoints, data-op the resting
        // opacity to restore to.
        double edge_op = 0.20;
        char opb[16];
        std::snprintf(opb, sizeof opb, "%.2f", edge_op);
        std::string ops = opb;
        out += "<g class=\"hv edge\" data-a=\"" + std::to_string(e.from) + "\" data-b=\""
            + std::to_string(e.to) + "\" data-op=\"" + ops + "\" opacity=\"" + ops
            + "\" data-crit=\"" + attr_num(share)
            + "\" data-kind=\"" + (e.explicit_ordering ? "explicit" : "derived")
            + "\" data-hl=\"" + stroke + "\" data-tip=\"";
        append_tip_attr(tip);
        out += "\">\n";
        if (weld)
        {
            double cx = (x1 + x2) * 0.5;
            // Invisible fat twin first (hover target), then the visible dot.
            line("<circle cx=\"%.1f\" cy=\"%.1f\" r=\"9\" fill=\"transparent\"/>\n", cx, y1);
            line("<circle cx=\"%.1f\" cy=\"%.1f\" r=\"4.5\" fill=\"%s\"/>\n", cx, y1, stroke.c_str());
        }
        else
        {
            // Invisible fat twin of the curve: a comfortable hover target for a 2px stroke.
            line("<path d=\"M %.1f %.1f C %.1f %.1f, %.1f %.1f, %.1f %.1f\" fill=\"none\" "
                 "stroke=\"transparent\" stroke-width=\"9\"/>\n",
                 x1, y1, x1 + tangent, y1, x2 - tangent, y2, x2, y2);
            line("<path d=\"M %.1f %.1f C %.1f %.1f, %.1f %.1f, %.1f %.1f\" fill=\"none\" "
                 "stroke=\"%s\" stroke-width=\"%.1f\"%s/>\n",
                 x1, y1, x1 + tangent, y1, x2 - tangent, y2, x2, y2, stroke.c_str(), width, extra);
            // Arrowhead at the entry, horizontal (consistent with the flat arrival).
            line("<polygon points=\"%.1f,%.1f %.1f,%.1f %.1f,%.1f\" fill=\"%s\"/>\n",
                 x2 - 7.0, y2 - 3.5, x2 - 7.0, y2 + 3.5, x2 - 0.5, y2, stroke.c_str());
        }
        out += "</g>\n";
    }

    // Hover-tooltip overlay: renders each element's `data-tip` lines styled (headline in
    // the element's own colour, field names bold). Runs when the SVG is opened as a
    // document (browser tab, <object>, <iframe>); inert when embedded via <img> --
    // acceptable, the picture still stands alone.
    out += "<style>.hv{cursor:default}.term{cursor:help}</style>\n";
    out += "<script><![CDATA[\n"
        "(function(){\n"
        "var svg=document.documentElement;\n"
        "var NS='http://www.w3.org/2000/svg';\n"
        // Glossary pills: draw a rounded rect behind each header term at load time. getBBox gives
        // the term's exact box in the header text's coordinate space (a direct root child, no group
        // transform); inserting the rect before its parent <text> paints it behind the text.
        "Array.prototype.forEach.call(document.querySelectorAll('.term'),function(el){\n"
        "  var b=el.getBBox();var r=document.createElementNS(NS,'rect');\n"
        "  r.setAttribute('x',(b.x-4).toFixed(1));r.setAttribute('y',(b.y+2).toFixed(1));\n"
        "  r.setAttribute('width',(b.width+8).toFixed(1));r.setAttribute('height',(b.height).toFixed(1));\n"
        "  r.setAttribute('rx','4');r.setAttribute('fill','#4d4436');r.setAttribute('pointer-events','none');\n"
        "  r.setAttribute('stroke','#ffffff');r.setAttribute('stroke-width','0.6');r.setAttribute('stroke-opacity','0.35');\n"
        "  var tp=el.parentNode;tp.parentNode.insertBefore(r,tp);\n"
        "});\n"
        "var tt=document.createElementNS(NS,'g');\n"
        "tt.setAttribute('visibility','hidden');tt.setAttribute('pointer-events','none');tt.setAttribute('opacity','0.8');\n"
        "var bg=document.createElementNS(NS,'rect');\n"
        "bg.setAttribute('fill','#1d1e19');bg.setAttribute('fill-opacity','0.96');\n"
        "bg.setAttribute('stroke','#66d9ef');bg.setAttribute('rx','4');\n"
        "var tx=document.createElementNS(NS,'text');\n"
        "tx.setAttribute('font-size','11');tx.setAttribute('font-family',\"'Segoe UI', sans-serif\");\n"
        "tt.appendChild(bg);tt.appendChild(tx);svg.appendChild(tt);\n"
        // Render a run of text into `parent`, splitting on backtick: `RRGGBBtext` chunks
        // (odd indices) are coloured, the rest use `def`.
        "function segs(parent,text,def){\n"
        "  var parts=text.split('`');\n"
        "  for(var j=0;j<parts.length;j++){\n"
        "    var s=document.createElementNS(NS,'tspan');\n"
        "    if(j%2===0){if(!parts[j])continue;s.setAttribute('fill',def);s.textContent=parts[j];}\n"
        "    else{var t2=parts[j].slice(6);if(!t2)continue;s.setAttribute('fill','#'+parts[j].slice(0,6));s.textContent=t2;}\n"
        "    parent.appendChild(s);\n"
        "  }\n"
        "}\n"
        "function show(el,evt){\n"
        "  while(tx.firstChild)tx.removeChild(tx.firstChild);\n"
        "  var lines=(el.getAttribute('data-tip')||'').split('\\n');\n"
        "  var hl=el.getAttribute('data-hl')||'#66d9ef';\n"
        "  var nc=el.getAttribute('data-nc')||hl;\n"
        "  bg.setAttribute('stroke',hl);\n"
        "  var name=null;\n"
        "  for(var i=0;i<lines.length;i++){\n"
        "    var t=document.createElementNS(NS,'tspan');\n"
        "    t.setAttribute('x','8');t.setAttribute('dy',i===0?'17':'14');\n"
        "    if(i===0){\n"
        "      t.setAttribute('font-weight','700');t.setAttribute('font-size','13');\n"
        "      t.setAttribute('fill',nc);t.textContent=lines[i];name=t;\n"
        "    }else{\n"
        "      var bt=lines[i].indexOf('`');var p=lines[i].indexOf(': ');\n"
        "      if(p>0&&(bt<0||bt>p)){\n"
        "        var b=document.createElementNS(NS,'tspan');\n"
        "        b.setAttribute('font-weight','700');b.setAttribute('fill','#f8f8f2');\n"
        "        b.textContent=lines[i].slice(0,p+1);t.appendChild(b);\n"
        "        segs(t,' '+lines[i].slice(p+2),'#cfcfc2');\n"
        "      }else if(lines[i].indexOf('->|')===0||lines[i].indexOf('|->')===0){\n"
        // Edge-list line: bold the ->| / |-> marker, colour the names via segments.
        "        var cut=(bt<0?lines[i].length:bt);\n"
        "        var mk=document.createElementNS(NS,'tspan');\n"
        "        mk.setAttribute('font-weight','700');mk.setAttribute('fill','#cfcfc2');\n"
        "        mk.textContent=lines[i].slice(0,cut);t.appendChild(mk);\n"
        "        segs(t,lines[i].slice(cut),'#cfcfc2');\n"
        "      }else{segs(t,lines[i],'#cfcfc2');}\n"
        "    }\n"
        "    tx.appendChild(t);\n"
        "  }\n"
        "  tt.setAttribute('visibility','visible');\n"
        // Priority tag right-aligned on the name line (nodes only; edges have no data-prio).
        "  var prio=el.getAttribute('data-prio');\n"
        "  if(prio&&name){\n"
        "    var b0=tx.getBBox();var pr=document.createElementNS(NS,'tspan');\n"
        "    pr.setAttribute('y','17');pr.setAttribute('font-weight','700');pr.setAttribute('font-size','12');\n"
        "    pr.setAttribute('text-anchor','end');pr.setAttribute('fill',el.getAttribute('data-pc')||nc);\n"
        "    pr.textContent=prio;tx.appendChild(pr);\n"
        "    var rt=Math.max(b0.x+b0.width, 8+name.getComputedTextLength()+18+pr.getComputedTextLength());\n"
        "    pr.setAttribute('x',rt);\n"
        "  }\n"
        "  move(evt);\n"
        "}\n"
        "function move(evt){\n"
        "  var pt=svg.createSVGPoint();pt.x=evt.clientX;pt.y=evt.clientY;\n"
        "  var m=svg.getScreenCTM();if(m)pt=pt.matrixTransform(m.inverse());\n"
        "  var b=tx.getBBox();var w=b.width+16,h=b.height+12;\n"
        "  bg.setAttribute('width',w);bg.setAttribute('height',h);\n"
        "  var vb=svg.viewBox.baseVal;\n"
        "  var x=pt.x+14,y=pt.y+14;\n"
        "  if(x+w>vb.width-4)x=pt.x-w-10;if(x<4)x=4;\n"
        "  if(y+h>vb.height-4)y=pt.y-h-10;if(y<4)y=4;\n"
        "  tt.setAttribute('transform','translate('+x+','+y+')');\n"
        "}\n"
        // Edges sink BEHIND the bars at load (they read as clutter on top otherwise); a node's
        // incident edges rise to a foreground layer (just under the tooltip) and brighten on
        // hover, then sink back on leave. An edge hovered directly also rises + brightens.
        "var edges=document.querySelectorAll('.edge');\n"
        "var bars=document.querySelectorAll('.hv[data-node]');\n"
        "var anchor=bars.length?bars[0]:null;\n"
        "var fg=document.createElementNS(NS,'g');svg.insertBefore(fg,tt);\n"
        "function sink(e){if(anchor)anchor.parentNode.insertBefore(e,anchor);}\n"
        "if(anchor)Array.prototype.forEach.call(edges,function(e){sink(e);});\n"
        "function hiEdges(id){Array.prototype.forEach.call(edges,function(e){if((e.getAttribute('data-a')===id||e.getAttribute('data-b')===id)&&parseFloat(e.getAttribute('data-op'))>0){e.setAttribute('opacity','1');e.setAttribute('pointer-events','none');fg.appendChild(e);}});}\n"
        "function loEdges(){while(fg.firstChild){var c=fg.firstChild;c.setAttribute('opacity',c.getAttribute('data-op'));c.setAttribute('pointer-events','auto');sink(c);}}\n"
        "Array.prototype.forEach.call(document.querySelectorAll('.hv'),function(el){\n"
        "  el.addEventListener('mouseenter',function(e){loEdges();show(el,e);var n=el.getAttribute('data-node');\n"
        "    if(n!==null)hiEdges(n);else if(el.getAttribute('data-a')!==null){el.setAttribute('opacity','1');fg.appendChild(el);}});\n"
        "  el.addEventListener('mousemove',move);\n"
        "  el.addEventListener('mouseleave',function(){tt.setAttribute('visibility','hidden');loEdges();});\n"
        "});\n"
        // ---- View bar: restyle toggles/cycles wired to the .btn buttons. A single
        // restyle() recomputes every element's target opacity from the current view state
        // (so views compose and never fight the hover edge-raise: it sets each edge's
        // data-op resting opacity, which hiEdges/loEdges already honour). ----
        "var critView=false,edgesOn=true,kindMode=0,coneMode=false,coneNode=null,deadView=false,metricMode=0;\n"
        "var deadbands=document.querySelectorAll('.deadband');\n"
        // Capture immutable originals before any restyle mutates them.
        "Array.prototype.forEach.call(edges,function(e){e._op0=parseFloat(e.getAttribute('data-op'));});\n"
        "Array.prototype.forEach.call(deadbands,function(r){r._op0=parseFloat(r.getAttribute('opacity'))||0.1;});\n"
        "Array.prototype.forEach.call(bars,function(g){var r=g.querySelector('rect');if(r)g._st0=r.getAttribute('stroke');});\n"
        // Adjacency for the dependency cone (ancestors via in-edges, descendants via out-edges).
        "var inAdj={},outAdj={};\n"
        "Array.prototype.forEach.call(edges,function(e){var a=e.getAttribute('data-a'),b=e.getAttribute('data-b');(outAdj[a]=outAdj[a]||[]).push(b);(inAdj[b]=inAdj[b]||[]).push(a);});\n"
        "function walk(start,adj){var seen={},st=[start];while(st.length){var n=st.pop();var nb=adj[n]||[];for(var i=0;i<nb.length;i++){if(!seen[nb[i]]){seen[nb[i]]=1;st.push(nb[i]);}}}return seen;}\n"
        "function cone(id){var s={};s[id]=1;var up=walk(id,inAdj),dn=walk(id,outAdj);for(var k in up)s[k]=1;for(var k2 in dn)s[k2]=1;return s;}\n"
        // green(0) -> yellow -> red(1) ramp, matching the util wash palette.
        "function ramp(t){t=Math.max(0,Math.min(1,t));var a,b,f;if(t<0.5){a=[0xa6,0xe2,0x2e];b=[0xe6,0xdb,0x74];f=t*2;}else{a=[0xe6,0xdb,0x74];b=[0xff,0x5f,0x45];f=(t-0.5)*2;}var r=Math.round(a[0]+(b[0]-a[0])*f),g=Math.round(a[1]+(b[1]-a[1])*f),c=Math.round(a[2]+(b[2]-a[2])*f);return '#'+((1<<24)+(r<<16)+(g<<8)+c).toString(16).slice(1);}\n"
        "function paintMetric(){\n"
        "  if(metricMode===0){Array.prototype.forEach.call(bars,function(g){var r=g.querySelector('rect');if(r&&g._st0)r.setAttribute('stroke',g._st0);});return;}\n"
        "  var attr=metricMode===1?'data-dispatch':'data-slack';var mx=0;\n"
        "  Array.prototype.forEach.call(bars,function(g){mx=Math.max(mx,parseFloat(g.getAttribute(attr))||0);});\n"
        "  if(mx<=0)mx=1;\n"
        // dispatch: long wait -> red; slack: tight (low) slack -> red (invert).
        "  Array.prototype.forEach.call(bars,function(g){var v=(parseFloat(g.getAttribute(attr))||0)/mx;var t=metricMode===2?1-v:v;var r=g.querySelector('rect');if(r)r.setAttribute('stroke',ramp(t));});\n"
        "}\n"
        "function restyle(){\n"
        "  var cs=(coneMode&&coneNode!=null)?cone(coneNode):null;\n"
        "  Array.prototype.forEach.call(bars,function(g){var op=1;\n"
        "    if(critView&&(parseFloat(g.getAttribute('data-crit'))||0)<0.15)op=Math.min(op,0.15);\n"
        "    if(cs&&!cs[g.getAttribute('data-node')])op=Math.min(op,0.12);\n"
        "    if(deadView)op=Math.min(op,0.30);\n"
        "    g.setAttribute('opacity',op);});\n"
        "  Array.prototype.forEach.call(edges,function(e){var op=e._op0;\n"
        "    var a=e.getAttribute('data-a'),b=e.getAttribute('data-b');\n"
        "    if(!edgesOn)op=0;\n"
        "    if(kindMode===1&&e.getAttribute('data-kind')!=='explicit')op=0;\n"
        "    if(kindMode===2&&e.getAttribute('data-kind')!=='derived')op=0;\n"
        "    if(critView&&(parseFloat(e.getAttribute('data-crit'))||0)<0.15)op=Math.min(op,0.03);\n"
        "    if(cs&&!(cs[a]&&cs[b]))op=Math.min(op,0.03);\n"
        "    if(deadView)op=Math.min(op,0.04);\n"
        "    e.setAttribute('data-op',op);if(e.parentNode!==fg)e.setAttribute('opacity',op);});\n"
        "  Array.prototype.forEach.call(deadbands,function(r){r.setAttribute('opacity',deadView?Math.min(1,r._op0*3+0.3):r._op0);});\n"
        "  paintMetric();\n"
        "}\n"
        "function setActive(b,on){var r=b.querySelector('rect');if(!r)return;r.setAttribute('fill',on?'#75715e':'#3e3d32');r.setAttribute('stroke',on?'#a6e22e':'#75715e');}\n"
        "function setLabel(b,t){var x=b.querySelector('text');if(x)x.textContent=t;}\n"
        "Array.prototype.forEach.call(document.querySelectorAll('.btn'),function(b){\n"
        "  b.addEventListener('click',function(ev){ev.stopPropagation();var v=b.getAttribute('data-view');\n"
        "    if(v==='crit'){critView=!critView;setActive(b,critView);}\n"
        "    else if(v==='edges'){edgesOn=!edgesOn;setActive(b,!edgesOn);setLabel(b,'Edges: '+(edgesOn?'on':'off'));}\n"
        "    else if(v==='kind'){kindMode=(kindMode+1)%3;setActive(b,kindMode!==0);setLabel(b,'Edge type: '+['all','explicit','derived'][kindMode]);}\n"
        "    else if(v==='cone'){coneMode=!coneMode;if(!coneMode)coneNode=null;setActive(b,coneMode);setLabel(b,'Cone: '+(coneMode?'pick':'off'));}\n"
        "    else if(v==='dead'){deadView=!deadView;setActive(b,deadView);}\n"
        "    else if(v==='metric'){metricMode=(metricMode+1)%3;setActive(b,metricMode!==0);setLabel(b,'Colour: '+['crit','dispatch','slack'][metricMode]);}\n"
        "    restyle();});\n"
        "});\n"
        // Cone selection: with cone mode on, a node click isolates its ancestors+descendants;
        // a background click clears the pick. stopPropagation on the node keeps the same click
        // from immediately clearing.
        "var coneBtn=document.querySelector('.btn[data-view=\"cone\"]');\n"
        "Array.prototype.forEach.call(bars,function(g){g.addEventListener('click',function(ev){if(coneMode){coneNode=g.getAttribute('data-node');ev.stopPropagation();if(coneBtn)setLabel(coneBtn,'Cone: set');restyle();}});});\n"
        "svg.addEventListener('click',function(){if(coneMode&&coneNode!=null){coneNode=null;if(coneBtn)setLabel(coneBtn,'Cone: pick');restyle();}});\n"
        "})();\n"
        "]]></script>\n";

    out += "</svg>\n";

    std::ofstream file(path, std::ios::binary);
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    if (!file)
    {
        std::fprintf(stderr, "Graph_trace: cannot write '%s'\n", path);
        return false;
    }
    return true;
}

} // namespace ts::tools
