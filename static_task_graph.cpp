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
        , remaining_objects(node_count)
        , launched(node_count)
        , remaining_accessors(pipe_count)
        , object_initiated(pipe_count)
    {}

    Static_task_graph* graph = nullptr;
    Scheduler* scheduler = nullptr;
    Cancellation_token token;
    std::vector<std::atomic<int>> remaining_deps;        // per node: unmet data prerequisites
    std::vector<std::atomic<int>> remaining_objects;     // per node: not-yet-reserved objects
    std::vector<std::atomic<int>> launched;              // per node: run-once guard (0/1)
    std::vector<std::atomic<int>> remaining_accessors;   // per pipe: accessors yet to complete (release at 0)
    std::vector<std::atomic<int>> object_initiated;      // per pipe: reservation initiated (0/1)
    std::atomic<int> remaining_nodes{ 0 };
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
    // touches and, per pipe, the list of nodes that access it (drives reservation and
    // early release).
    std::map<detail::Pipe*, int> index_of;
    for (int i = 0; i < static_cast<int>(distinct_pipes_.size()); ++i)
        index_of[distinct_pipes_[i]] = i;

    pipe_accessors_.assign(distinct_pipes_.size(), {});
    for (int n = 0; n < static_cast<int>(nodes_.size()); ++n)
    {
        std::set<int> indices;   // dedup: a node counts once per pipe even if it lists it twice
        for (detail::Pipe* p : nodes_[n].pipes)
            indices.insert(index_of[p]);
        nodes_[n].pipe_indices.assign(indices.begin(), indices.end());
        for (int idx : nodes_[n].pipe_indices)
            pipe_accessors_[idx].push_back(n);
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

// A node has become data-ready (all data prerequisites met). Lazily reserve the
// objects it touches (the first data-ready accessor of each object triggers its
// reservation), then check whether the node can run.
void Static_task_graph::on_data_ready(const std::shared_ptr<Run_state>& run, int index)
{
    for (int pi : run->graph->nodes_[index].pipe_indices)
        ensure_reserved(run, pi);
    maybe_run(run, index);
}

// Reserve a pipe once (whichever data-ready accessor gets here first). When acquired,
// notify every accessor so their reservation counts drop.
void Static_task_graph::ensure_reserved(const std::shared_ptr<Run_state>& run, int pipe_index)
{
    if (run->object_initiated[pipe_index].exchange(1, std::memory_order_acq_rel) != 0)
        return;   // another accessor already initiated it

    bool acquired = detail::pipe_reserve(*run->scheduler, *run->graph->distinct_pipes_[pipe_index],
        [run, pipe_index] { on_object_reserved(run, pipe_index); });

    if (acquired)
        on_object_reserved(run, pipe_index);
}

// A pipe's reservation is now held: every node that accesses it has one fewer
// outstanding reservation, and may become runnable.
void Static_task_graph::on_object_reserved(const std::shared_ptr<Run_state>& run, int pipe_index)
{
    for (int node : run->graph->pipe_accessors_[pipe_index])
    {
        run->remaining_objects[node].fetch_sub(1, std::memory_order_acq_rel);
        maybe_run(run, node);
    }
}

// Run the node iff both gates are open (data deps met and all its objects reserved),
// exactly once.
void Static_task_graph::maybe_run(const std::shared_ptr<Run_state>& run, int index)
{
    if (run->remaining_deps[index].load(std::memory_order_acquire) != 0
        || run->remaining_objects[index].load(std::memory_order_acquire) != 0)
        return;
    if (run->launched[index].exchange(1, std::memory_order_acq_rel) == 0)
        run_node(run, index);
}

void Static_task_graph::run_node(const std::shared_ptr<Run_state>& run, int index)
{
    detail::submit_closure(*run->scheduler, [run, index]
    {
        Node& node = run->graph->nodes_[index];

        if (!run->token.is_cancel_requested())   // cancelled: skip the body, keep draining
            node.run();

        // Early release: free each object this node was the last to touch, so queued
        // async on it can run. Safe -- the count hits 0 only after every node
        // accessing that object has completed.
        for (int pi : node.pipe_indices)
            if (run->remaining_accessors[pi].fetch_sub(1, std::memory_order_acq_rel) == 1)
                detail::pipe_release(*run->scheduler, *run->graph->distinct_pipes_[pi]);

        for (int successor : node.successors)
            if (run->remaining_deps[successor].fetch_sub(1, std::memory_order_acq_rel) == 1)
                on_data_ready(run, successor);

        if (run->remaining_nodes.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            if (run->token.is_cancel_requested())
                run->done->cancel();
            else
                run->done->complete();
        }
    });
}

Task<void> Static_task_graph::execute(Scheduler& scheduler, Cancellation_token token)
{
    if (!compiled_)
        ts::fatal("Static_task_graph::execute called before compile()");

    auto run = std::make_shared<Run_state>(nodes_.size(), distinct_pipes_.size());
    run->graph = this;
    run->scheduler = &scheduler;
    run->token = token;
    for (size_t i = 0; i < nodes_.size(); ++i)
    {
        run->remaining_deps[i].store(nodes_[i].indegree, std::memory_order_relaxed);
        run->remaining_objects[i].store(static_cast<int>(nodes_[i].pipe_indices.size()), std::memory_order_relaxed);
    }
    for (size_t i = 0; i < distinct_pipes_.size(); ++i)
        run->remaining_accessors[i].store(static_cast<int>(pipe_accessors_[i].size()), std::memory_order_relaxed);
    run->remaining_nodes.store(static_cast<int>(nodes_.size()), std::memory_order_relaxed);
    run->done = std::make_shared<detail::Task_control_block<void>>();

    Task<void> result(run->done);

    if (nodes_.empty())
    {
        run->done->complete();
        return result;
    }

    // Objects are reserved lazily (see on_data_ready), so a graph object is held only
    // from its first accessor's dispatch to its last accessor's completion -- not the
    // whole run. Kick off every root (indegree 0).
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].indegree == 0)
            on_data_ready(run, static_cast<int>(i));

    return result;
}

} // namespace ts
