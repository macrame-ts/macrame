#include "static_task_graph.h"
#include "fatal.h"

#include <atomic>
#include <deque>
#include <map>
#include <set>

namespace ts
{

struct Static_task_graph::Run_state
{
    Run_state(size_t node_count, size_t pipe_count)
        : remaining_deps(node_count)
        , remaining_accessors(pipe_count)
    {}

    Static_task_graph* graph = nullptr;
    Scheduler* scheduler = nullptr;
    std::vector<std::atomic<int>> remaining_deps;
    std::vector<std::atomic<int>> remaining_accessors;   // per distinct pipe; release at 0
    std::atomic<int> remaining_nodes{ 0 };
    std::atomic<int> pending_reservations{ 0 };
    std::shared_ptr<detail::Task_control_block<void>> done;
};

void Static_task_graph::add_edge(int prerequisite, int successor)
{
    explicit_edges_.emplace_back(prerequisite, successor);
    compiled_ = false;
}

Graph_node& Graph_node::after(const Graph_node& prerequisite)
{
    if (graph_ && graph_ == prerequisite.graph_)
        graph_->add_edge(prerequisite.index_, index_);
    return *this;
}

Graph_node& Graph_node::before(const Graph_node& successor)
{
    if (graph_ && graph_ == successor.graph_)
        graph_->add_edge(index_, successor.index_);
    return *this;
}

// Two nodes conflict if they touch a common instance and at least one wants write.
bool Static_task_graph::conflicts(const Node& a, const Node& b)
{
    for (const auto& [instance_a, mode_a] : a.access)
        for (const auto& [instance_b, mode_b] : b.access)
            if (instance_a == instance_b
                && (mode_a == Access::read_write || mode_b == Access::read_write))
                return true;
    return false;
}

void Static_task_graph::compile()
{
    for (Node& node : nodes_)
    {
        node.successors.clear();
        node.indegree = 0;
    }

    // dedup edges across explicit + access-conflict sources
    std::set<std::pair<int, int>> edges;

    for (const auto& edge : explicit_edges_)
        edges.insert(edge);

    // conflicting nodes are ordered by declaration index (deterministic tiebreak;
    // ambiguity reporting is future work, see docs/TODO.md)
    for (int i = 0; i < static_cast<int>(nodes_.size()); ++i)
        for (int j = i + 1; j < static_cast<int>(nodes_.size()); ++j)
            if (conflicts(nodes_[i], nodes_[j]))
                edges.insert({ i, j });

    for (const auto& [from, to] : edges)
    {
        nodes_[from].successors.push_back(to);
        ++nodes_[to].indegree;
    }

    // Distinct set of pipes the graph touches, for per-run reservation (see execute()).
    std::set<detail::Pipe*> pipes;
    for (const Node& node : nodes_)
        for (detail::Pipe* p : node.pipes)
            pipes.insert(p);
    distinct_pipes_.assign(pipes.begin(), pipes.end());

    // Map each pipe to its index, then record per node the (deduped) pipe indices it
    // touches and, per pipe, how many nodes touch it (the per-run accessor count that
    // drives early release).
    std::map<detail::Pipe*, int> index_of;
    for (int i = 0; i < static_cast<int>(distinct_pipes_.size()); ++i)
        index_of[distinct_pipes_[i]] = i;

    pipe_accessor_counts_.assign(distinct_pipes_.size(), 0);
    for (Node& node : nodes_)
    {
        std::set<int> indices;   // dedup: a node counts once per pipe even if it lists it twice
        for (detail::Pipe* p : node.pipes)
            indices.insert(index_of[p]);
        node.pipe_indices.assign(indices.begin(), indices.end());
        for (int idx : node.pipe_indices)
            ++pipe_accessor_counts_[idx];
    }

    detect_cycles();
    compiled_ = true;
}

void Static_task_graph::detect_cycles() const
{
    std::vector<int> indegree(nodes_.size());
    for (size_t i = 0; i < nodes_.size(); ++i)
        indegree[i] = nodes_[i].indegree;

    std::deque<int> ready;
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (indegree[i] == 0)
            ready.push_back(static_cast<int>(i));

    size_t visited = 0;
    while (!ready.empty())
    {
        int n = ready.front();
        ready.pop_front();
        ++visited;
        for (int s : nodes_[n].successors)
            if (--indegree[s] == 0)
                ready.push_back(s);
    }

    if (visited != nodes_.size())
        ts::fatal("Static_task_graph has a cycle");
}

void Static_task_graph::run_node(const std::shared_ptr<Run_state>& run, int index)
{
    detail::submit_closure(*run->scheduler, [run, index]
    {
        Node& node = run->graph->nodes_[index];
        node.run();

        // Early release: free each object this node was the last to touch, so queued
        // async on it can run without waiting for the whole graph. Safe -- the count
        // hits 0 only after every node accessing that object has completed.
        for (int pi : node.pipe_indices)
            if (run->remaining_accessors[pi].fetch_sub(1, std::memory_order_acq_rel) == 1)
                detail::pipe_release(*run->scheduler, *run->graph->distinct_pipes_[pi]);

        for (int successor : node.successors)
            if (run->remaining_deps[successor].fetch_sub(1, std::memory_order_acq_rel) == 1)
                run_node(run, successor);

        if (run->remaining_nodes.fetch_sub(1, std::memory_order_acq_rel) == 1)
            run->done->complete();
    });
}

// Start every root (indegree-0) node. Called once all object reservations are held.
void Static_task_graph::start_roots(const std::shared_ptr<Run_state>& run)
{
    const Static_task_graph* graph = run->graph;
    for (size_t i = 0; i < graph->nodes_.size(); ++i)
        if (graph->nodes_[i].indegree == 0)
            run_node(run, static_cast<int>(i));
}

Task<void> Static_task_graph::execute(Scheduler& scheduler)
{
    if (!compiled_)
        ts::fatal("Static_task_graph::execute called before compile()");

    auto run = std::make_shared<Run_state>(nodes_.size(), distinct_pipes_.size());
    run->graph = this;
    run->scheduler = &scheduler;
    for (size_t i = 0; i < nodes_.size(); ++i)
        run->remaining_deps[i].store(nodes_[i].indegree, std::memory_order_relaxed);
    for (size_t i = 0; i < distinct_pipes_.size(); ++i)
        run->remaining_accessors[i].store(pipe_accessor_counts_[i], std::memory_order_relaxed);
    run->remaining_nodes.store(static_cast<int>(nodes_.size()), std::memory_order_relaxed);
    run->done = std::make_shared<detail::Task_control_block<void>>();

    Task<void> result(run->done);

    if (nodes_.empty())
    {
        run->done->complete();
        return result;
    }

    // Reserve every object the graph touches before running any node, so a node's
    // direct (pipe-bypassing) access can't race a dynamic `async` on the same object.
    // Nodes start only once all reservations are held; each object is released early,
    // by its last accessor (see run_node), not at whole-run completion. A pipe
    // reserved synchronously counts down inline; a deferred one from its callback.
    run->pending_reservations.store(static_cast<int>(distinct_pipes_.size()), std::memory_order_relaxed);

    if (distinct_pipes_.empty())
    {
        start_roots(run);
        return result;
    }

    for (detail::Pipe* p : distinct_pipes_)
    {
        bool acquired = detail::pipe_reserve(scheduler, *p, [run]
        {
            if (run->pending_reservations.fetch_sub(1, std::memory_order_acq_rel) == 1)
                start_roots(run);
        });

        if (acquired
            && run->pending_reservations.fetch_sub(1, std::memory_order_acq_rel) == 1)
            start_roots(run);
    }

    return result;
}

} // namespace ts
