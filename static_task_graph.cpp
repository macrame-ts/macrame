#include "static_task_graph.h"
#include "fatal.h"

#include <atomic>
#include <deque>
#include <set>

namespace ts
{

struct Static_task_graph::Run_state
{
    explicit Run_state(size_t count)
        : remaining_deps(count)
    {}

    Static_task_graph* graph = nullptr;
    Scheduler* scheduler = nullptr;
    std::vector<std::atomic<int>> remaining_deps;
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
        run->graph->nodes_[index].run();

        for (int successor : run->graph->nodes_[index].successors)
            if (run->remaining_deps[successor].fetch_sub(1, std::memory_order_acq_rel) == 1)
                run_node(run, successor);

        if (run->remaining_nodes.fetch_sub(1, std::memory_order_acq_rel) == 1)
            run->done->complete();
    });
}

Task<void> Static_task_graph::execute(Scheduler& scheduler)
{
    if (!compiled_)
        ts::fatal("Static_task_graph::execute called before compile()");

    auto run = std::make_shared<Run_state>(nodes_.size());
    run->graph = this;
    run->scheduler = &scheduler;
    for (size_t i = 0; i < nodes_.size(); ++i)
        run->remaining_deps[i].store(nodes_[i].indegree, std::memory_order_relaxed);
    run->remaining_nodes.store(static_cast<int>(nodes_.size()), std::memory_order_relaxed);
    run->done = std::make_shared<detail::Task_control_block<void>>();

    Task<void> result(run->done);

    if (nodes_.empty())
    {
        run->done->complete();
        return result;
    }

    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].indegree == 0)
            run_node(run, static_cast<int>(i));

    return result;
}

} // namespace ts
