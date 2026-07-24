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
    void on_run_complete(const long long* readys, const long long* starts,
                         const long long* ends, const int* workers,
                         int node_count, long long run_begin, long long run_end,
                         long long busy_ticks, int worker_count)
    {
        if (node_count != static_cast<int>(nodes_.size()))
            return;   // structure not pushed (or a stale attach) -- drop the sample

        long long window = run_end - run_begin;
        if (worker_count > 0 && window > 0)
            core_util_.add(std::clamp(
                static_cast<double>(busy_ticks)
                    / (static_cast<double>(worker_count) * static_cast<double>(window)),
                0.0, 1.0));

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
            a.dispatch_wait.add(std::max(0.0,
                static_cast<double>(starts[i] - readys[i]) * ticks_to_us));
            int w = workers[i];
            if (w < 0)
                ++a.external_runs;
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
            a.critical_runs = 0;
        }
        for (Edge_agg& e : edges_)
        {
            e.meet = {};
            e.binding_gap = {};
            e.critical_runs = 0;
        }
        critical_work_ = {};
        makespan_ = {};
        makespan_min_ = 0.0;
        makespan_max_ = 0.0;
        core_util_ = {};
        runs_ = 0;
    }

    long long run_count() const { return runs_; }
    int structure_node_count() const { return static_cast<int>(nodes_.size()); }

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
    };

    Node_stats node_stats(int index) const
    {
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
        return s;
    }

    // Optional title prefix for the rendered SVG (e.g. `Sample "game_frame"` -> the
    // header reads `Sample "game_frame": average run`).
    void set_title(std::string title)
    {
        title_ = std::move(title);
    }

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
            if (ends[i] > ends[cur])
                cur = i;

        double work = 0.0;
        while (cur >= 0)
        {
            ++nodes_[static_cast<size_t>(cur)].critical_runs;
            work += static_cast<double>(ends[cur] - starts[cur]) * ticks_to_us;

            int binding_edge = -1;
            for (int e : in_edges_[static_cast<size_t>(cur)])
                if (binding_edge < 0
                    || ends[edges_[static_cast<size_t>(e)].from]
                     > ends[edges_[static_cast<size_t>(binding_edge)].from])
                    binding_edge = e;
            if (binding_edge < 0)
                break;   // a root: the chain is complete
            Edge_agg& be = edges_[static_cast<size_t>(binding_edge)];
            ++be.critical_runs;
            // The chain's dead time at this step: the successor sat ready-but-waiting
            // (queue / acquire) after its binding predecessor finished.
            be.binding_gap.add(std::max(0.0,
                static_cast<double>(starts[cur] - ends[be.from]) * ticks_to_us));
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
            if (a.worker_runs[w] > best)
            {
                best = a.worker_runs[w];
                worker = static_cast<int>(w);
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
    long long runs_ = 0;
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
        if (indegree[static_cast<size_t>(i)] == 0)
            topo.push_back(i);
    for (size_t h = 0; h < topo.size(); ++h)
        for (int e : out_edges[static_cast<size_t>(topo[h])])
            if (--indegree[static_cast<size_t>(edges_[static_cast<size_t>(e)].to)] == 0)
                topo.push_back(edges_[static_cast<size_t>(e)].to);

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
        for (int e : out_edges[static_cast<size_t>(u)])
        {
            size_t v = static_cast<size_t>(edges_[static_cast<size_t>(e)].to);
            est[v] = std::max(est[v], est[static_cast<size_t>(u)] + dur[static_cast<size_t>(u)]);
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
        slack[static_cast<size_t>(i)] = std::max(0.0,
            lf[static_cast<size_t>(i)] - (est[static_cast<size_t>(i)] + dur[static_cast<size_t>(i)]));

    for (int u : topo)
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
        workers_seen = std::max(workers_seen,
            static_cast<int>(nodes_[static_cast<size_t>(i)].worker_runs.size()));

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
    const double header_h = 84.0, axis_h = 30.0, legend_h = 168.0;
    const double row_h = 30.0, bar_h = 20.0;
    const double px_per_us = plot_w / span_us;

    const double rows_top = header_h;
    const double rows_bottom = rows_top + row_count * row_h;
    const double total_w = pad_l + plot_w + pad_r;
    const double total_h = rows_bottom + axis_h + legend_h;

    auto X = [&](double us) { return pad_l + us * px_per_us; };
    auto bar_top = [&](int i)
    {
        return rows_top + row_of[static_cast<size_t>(i)] * row_h + (row_h - bar_h) * 0.5;
    };

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
    out += "<defs><pattern id=\"dead\" width=\"6\" height=\"6\" patternUnits=\"userSpaceOnUse\">"
           "<rect width=\"6\" height=\"6\" fill=\"#f92672\" opacity=\"0.10\"/>"
           "<path d=\"M0 6 L6 0\" stroke=\"#f92672\" stroke-width=\"1.4\" opacity=\"0.7\"/>"
           "</pattern></defs>\n";

    // Header: title, the dead-time headline, global stats. (The legend sits at the
    // bottom of the picture.)
    out += "<text x=\"16\" y=\"26\" font-size=\"15\" font-weight=\"600\" fill=\"#f8f8f2\">";
    append_escaped(out, title_.empty() ? "average run" : title_ + ": average run");
    out += "</text>\n";
    {
        // The two frame classifiers on one coloured headline (band constants at the top of
        // this header): core utilization -- how much of the machine the frame used -- then
        // critical dead time (makespan minus the binding chain's work) -- whether the
        // chain itself had to wait.
        double util = core_util_.mean;
        const char* util_color = util >= core_util_good_share ? "#a6e22e"
                               : util >= core_util_ok_share ? "#e6db74" : "#ff5f45";
        double dead_us = std::max(0.0, makespan_.mean - critical_work_.mean);
        double dead_share = makespan_.mean > 0.0 ? dead_us / makespan_.mean : 0.0;
        const char* dead_color = dead_share < dead_time_ok_share ? "#a6e22e"
                               : dead_share <= dead_time_bad_share ? "#e6db74" : "#ff5f45";
        out += "<text x=\"16\" y=\"48\" font-size=\"12\" font-weight=\"600\">";
        out += "<tspan fill=\"" + std::string(util_color) + "\">";
        append_escaped(out, "core utilization: " + fmt_us(100.0 * util) + "%");
        out += "</tspan><tspan fill=\"#cfcfc2\"> | </tspan>";
        out += "<tspan fill=\"" + std::string(dead_color) + "\">";
        append_escaped(out, "critical dead time: " + fmt_us(dead_us) + " \xC2\xB5s ("
            + fmt_us(100.0 * dead_share) + "% of frame time)");
        out += "</tspan></text>\n";

        // "structural CP" = the CPM critical-path length: the dependency lower bound on
        // frame time from median durations and edges alone (no scheduling waits).
        std::string stats = "runs: " + std::to_string(runs_)
            + "  |  frame time mean " + fmt_ms(makespan_.mean) + " ms (min " + fmt_ms(makespan_min_)
            + ", max " + fmt_ms(makespan_max_) + ")  |  critical work mean "
            + fmt_ms(critical_work_.mean) + " ms  |  structural CP " + fmt_ms(cpm_us)
            + " ms  |  workers: " + std::to_string(workers_seen);
        out += "<text x=\"16\" y=\"68\" font-size=\"11\" fill=\"#cfcfc2\">";
        append_escaped(out, stats);
        out += "</text>\n";
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
                 X(t), header_h - 4.0, X(t), rows_bottom);
            std::string lbl = fmt_us(t) + " \xC2\xB5s";
            out += "<text x=\"" + std::to_string(X(t)) + "\" y=\"" + std::to_string(rows_bottom + 18.0)
                 + "\" font-size=\"10\" fill=\"#cfcfc2\" text-anchor=\"middle\">";
            append_escaped(out, lbl);
            out += "</text>\n";
        }
    }

    {
        // Legend, at the bottom (below the time axis): a short sample glyph, then its
        // explanation, inline. Line STYLE carries provenance (solid = explicit, dashed =
        // derived); colour carries criticality (green -> pink by share of binding
        // chains).
        const double ax0 = 16.0, ax1 = 56.0;
        const double base = rows_bottom + axis_h;
        auto legend_arrow = [&](double ly, const char* stroke, const char* dash, const char* text)
        {
            line("<line x1=\"%.0f\" y1=\"%.0f\" x2=\"%.0f\" y2=\"%.0f\" stroke=\"%s\" stroke-width=\"1.8\"%s/>\n",
                 ax0, ly, ax1, ly, stroke, dash);
            line("<polygon points=\"%.0f,%.0f %.0f,%.0f %.0f,%.0f\" fill=\"%s\"/>\n",
                 ax1, ly - 4.0, ax1, ly + 4.0, ax1 + 7.0, ly, stroke);
            line("<text x=\"%.0f\" y=\"%.0f\" font-size=\"11\" fill=\"#cfcfc2\">%s</text>\n",
                 ax1 + 14.0, ly + 4.0, text);
        };
        legend_arrow(base + 14.0, "#a6e22e", "", "explicit ordering (after/before)");
        legend_arrow(base + 32.0, "#a6e22e", " stroke-dasharray=\"5,3\"", "derived from declared access (hover for detail)");
        legend_arrow(base + 50.0, "#f92672", "", "critical-path edge (pink = share of runs)");
        const double ly4 = base + 68.0;
        line("<rect x=\"%.0f\" y=\"%.0f\" width=\"%.0f\" height=\"10\" rx=\"2\" fill=\"#3e3d32\" "
             "stroke=\"#fd971f\" stroke-width=\"2\"/>\n", ax0, ly4 - 5.0, ax1 - ax0);
        line("<text x=\"%.0f\" y=\"%.0f\" font-size=\"11\" fill=\"#cfcfc2\">critical node (orange = share of runs)</text>\n",
             ax1 + 14.0, ly4 + 4.0);
        const double ly5 = base + 86.0;
        line("<circle cx=\"%.0f\" cy=\"%.0f\" r=\"5\" fill=\"#a6e22e\"/>\n", (ax0 + ax1) * 0.5, ly5);
        line("<text x=\"%.0f\" y=\"%.0f\" font-size=\"11\" fill=\"#cfcfc2\">handoff weld: connected nodes back to back</text>\n",
             ax1 + 14.0, ly5 + 4.0);
        const double ly6 = base + 104.0;
        line("<rect x=\"%.0f\" y=\"%.0f\" width=\"%.0f\" height=\"10\" rx=\"2\" fill=\"url(#dead)\"/>\n",
             ax0, ly6 - 5.0, ax1 - ax0);
        line("<text x=\"%.0f\" y=\"%.0f\" font-size=\"11\" fill=\"#cfcfc2\">critical dead time (successor ready but waiting)</text>\n",
             ax1 + 14.0, ly6 + 4.0);
        line("<text x=\"%.0f\" y=\"%.0f\" font-size=\"11\">"
             "<tspan fill=\"%s\">high</tspan><tspan fill=\"#cfcfc2\"> / </tspan>"
             "<tspan fill=\"%s\">normal</tspan><tspan fill=\"#cfcfc2\"> / </tspan>"
             "<tspan fill=\"%s\">low</tspan>"
             "<tspan fill=\"#cfcfc2\"> = node name colour = priority</tspan></text>\n",
             ax0, base + 126.0,
             priority_color(Priority::high), priority_color(Priority::normal), priority_color(Priority::low));
        line("<text x=\"%.0f\" y=\"%.0f\" font-size=\"11\" fill=\"#cfcfc2\">"
             "edges are faint by default -- hover a node to highlight its dependencies</text>\n",
             ax0, base + 148.0);
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

    // Critical dead-time regions, in the BACKGROUND and FULL HEIGHT: the wait is a
    // property of the binding chain, not of any row or worker. For an edge that binds
    // >= 10% of runs, the region is the VISIBLE gap on screen between the predecessor's
    // drawn right edge and the successor's drawn left edge -- pure screen coordinates, so
    // it is always the real break between two bars and can never land on a bar (the old
    // form subtracted a MEAN gap from a MEDIAN bar start, mixing coordinate systems, and
    // could overlap a critical node). Overlapping regions from different edges are unioned,
    // so no vertical slice is painted twice. These localize the headline critical dead
    // time (frame_time_mean - critical_work_mean, computed globally) -- they are its
    // >= 10%-share visible subset, not a sum of it. Behind the bars, `pointer-events: none`,
    // so hover is never intercepted (the per-edge number lives on the edge tooltip).
    {
        std::vector<std::pair<double, double>> gaps;
        for (const Edge_agg& e : edges_)
        {
            double share = runs_ > 0
                ? static_cast<double>(e.critical_runs) / static_cast<double>(runs_) : 0.0;
            if (share < 0.10)
                continue;
            double x_pred = X(bar_start[static_cast<size_t>(e.from)])
                + std::max(3.0, (bar_end[static_cast<size_t>(e.from)]
                                 - bar_start[static_cast<size_t>(e.from)]) * px_per_us);
            double x_succ = X(bar_start[static_cast<size_t>(e.to)]);
            if (x_succ - x_pred > 2.0)
                gaps.emplace_back(x_pred, x_succ);
        }
        std::sort(gaps.begin(), gaps.end());
        double cur0 = 0.0, cur1 = -1.0;
        auto flush = [&]
        {
            if (cur1 > cur0)
                line("<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" "
                     "fill=\"url(#dead)\" pointer-events=\"none\"/>\n",
                     cur0, rows_top, cur1 - cur0, rows_bottom - rows_top);
        };
        for (const auto& g : gaps)
        {
            if (g.first > cur1)
            {
                flush();
                cur0 = g.first;
                cur1 = g.second;
            }
            else
                cur1 = std::max(cur1, g.second);
        }
        flush();
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
        out += "<g class=\"hv\" data-node=\"" + std::to_string(i) + "\" data-hl=\"" + color
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

    for (const Edge_agg& e : edges_)
    {
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

        // Edges are faint by default so the picture reads as bars first; a critical edge
        // stays prominent (opacity scales with its share of binding chains), and hovering
        // a node brightens all its incident edges to full (see the overlay). data-a/data-b
        // are the endpoints; data-op the resting opacity to restore to.
        double edge_op = 0.18 + 0.82 * crit;
        char opb[16];
        std::snprintf(opb, sizeof opb, "%.2f", edge_op);
        std::string ops = opb;
        out += "<g class=\"hv edge\" data-a=\"" + std::to_string(e.from) + "\" data-b=\""
            + std::to_string(e.to) + "\" data-op=\"" + ops + "\" opacity=\"" + ops
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
    out += "<style>.hv{cursor:default}</style>\n";
    out += "<script><![CDATA[\n"
        "(function(){\n"
        "var svg=document.documentElement;\n"
        "var NS='http://www.w3.org/2000/svg';\n"
        "var tt=document.createElementNS(NS,'g');\n"
        "tt.setAttribute('visibility','hidden');tt.setAttribute('pointer-events','none');\n"
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
        // Hover-scoped edges: faint by default (set per edge), a node's incident edges
        // brighten to full on hover, an edge brightens itself; leaving restores all.
        "var edges=document.querySelectorAll('.edge');\n"
        "function hiEdges(id){Array.prototype.forEach.call(edges,function(e){if(e.getAttribute('data-a')===id||e.getAttribute('data-b')===id)e.setAttribute('opacity','1');});}\n"
        "function loEdges(){Array.prototype.forEach.call(edges,function(e){e.setAttribute('opacity',e.getAttribute('data-op'));});}\n"
        "Array.prototype.forEach.call(document.querySelectorAll('.hv'),function(el){\n"
        "  el.addEventListener('mouseenter',function(e){show(el,e);var n=el.getAttribute('data-node');\n"
        "    if(n!==null)hiEdges(n);else if(el.getAttribute('data-a')!==null)el.setAttribute('opacity','1');});\n"
        "  el.addEventListener('mousemove',move);\n"
        "  el.addEventListener('mouseleave',function(){tt.setAttribute('visibility','hidden');loEdges();});\n"
        "});\n"
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
