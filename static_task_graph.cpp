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
    std::shared_ptr<detail::Task_control_block> done;
};

namespace
{

// A graph node's reusable task block: the monomorphic control block (FIRST member, so a
// `Task_control_block*` aliases / `reinterpret_cast`s back to it) plus the back-pointers
// its execute/on_complete hooks need to reach the node body and the run. Allocated once
// in compile(), re-armed each execute() -- so a run dispatches nodes with no per-node
// allocation. The body is NOT stored here (reached via graph->nodes_[index].run), so
// there is no per-run body closure either.
struct Graph_node_block
{
    detail::Task_control_block core;   // MUST be first
    Static_task_graph* graph = nullptr;
    int index = -1;
};

} // namespace

// Defaulted here, where Run_state is complete (the header's `run_` unique_ptr needs it).
Static_task_graph::Static_task_graph() = default;
Static_task_graph::~Static_task_graph() = default;
Static_task_graph::Static_task_graph(Static_task_graph&&) noexcept = default;
Static_task_graph& Static_task_graph::operator=(Static_task_graph&&) noexcept = default;

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

    // Each node runs as a reusable task block, allocated once here and re-armed per run
    // (§7.1) -- so a run dispatches every node without allocating. `execute`/`on_complete`
    // are fixed fn-ptrs; graph+index let those hooks reach the node body and the run.
    for (int i = 0; i < static_cast<int>(nodes_.size()); ++i)
    {
        auto wrapper = std::make_shared<Graph_node_block>();
        wrapper->graph = this;
        wrapper->index = i;
        wrapper->core.execute = &run_graph_node;
        wrapper->core.on_complete = &graph_node_completed;
        nodes_[i].block = std::shared_ptr<detail::Task_control_block>(wrapper, &wrapper->core);
    }

    // Reused per-run state: values are reset each execute(), vector capacity persists, so
    // a run allocates only its completion handle (`done`). Rebuilt here since node/pipe
    // counts can change between compiles.
    run_ = std::make_unique<Run_state>(nodes_.size(), distinct_pipes_.size());
    run_->graph = this;

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
void Static_task_graph::on_data_ready(Run_state& run, int index)
{
    for (int pi : run.graph->nodes_[index].pipe_indices)
        ensure_reserved(run, pi);
    maybe_run(run, index);
}

// Reserve a pipe once (whichever data-ready accessor gets here first). When acquired,
// notify every accessor so their reservation counts drop.
void Static_task_graph::ensure_reserved(Run_state& run, int pipe_index)
{
    if (run.object_initiated[pipe_index].exchange(1, std::memory_order_acq_rel) != 0)
        return;   // another accessor already initiated it

    Run_state* rp = &run;   // stable (run_ outlives the run); small capture -> no alloc
    bool acquired = detail::pipe_reserve(*run.scheduler, *run.graph->distinct_pipes_[pipe_index],
        [rp, pipe_index] { on_object_reserved(*rp, pipe_index); });

    if (acquired)
        on_object_reserved(run, pipe_index);
}

// A pipe's reservation is now held: every node that accesses it has one fewer
// outstanding reservation, and may become runnable.
void Static_task_graph::on_object_reserved(Run_state& run, int pipe_index)
{
    for (int node : run.graph->pipe_accessors_[pipe_index])
    {
        run.remaining_objects[node].fetch_sub(1, std::memory_order_acq_rel);
        maybe_run(run, node);
    }
}

// Run the node iff both gates are open (data deps met and all its objects reserved),
// exactly once.
void Static_task_graph::maybe_run(Run_state& run, int index)
{
    if (run.remaining_deps[index].load(std::memory_order_acquire) != 0
        || run.remaining_objects[index].load(std::memory_order_acquire) != 0)
        return;
    if (run.launched[index].exchange(1, std::memory_order_acq_rel) == 0)
        run_node(run, index);
}

// Dispatch a node: submit its (re-armed) block via the raw scheduler API -- a fn-ptr
// plus the Node address, so no per-node closure is allocated.
void Static_task_graph::run_node(Run_state& run, int index)
{
    run.scheduler->submit(&node_trampoline, &run.graph->nodes_[index]);
}

// Raw scheduler entry: run the node's block. Kept alive by the graph (Node owns the
// block); the trampoline holds no ownership.
void Static_task_graph::node_trampoline(void* node)
{
    auto* n = static_cast<Node*>(node);
    n->block->execute(n->block);
}

// The node block's `execute`. Mirrors `Executable::run`, but reaches the body via
// graph+index (no stored body) and completes via the block's `on_complete` hook: it
// installs `current_task` + the execution-flag self-lock so the body may spawn NESTED
// tasks (`ts::nested`) that gate the node's completion, then completes once the self-lock
// and all nested tasks release. The block carries the run's `token`, so a cancelled node
// skips its body (settling cancelled) -- `on_complete` still fires, keeping the drain
// going.
void Static_task_graph::run_graph_node(const std::shared_ptr<detail::Task_control_block>& block)
{
    using Block = detail::Task_control_block;

    if (block->started.exchange(true, std::memory_order_acq_rel))
        return;   // already claimed (belt-and-suspenders; graph nodes dispatch once)

    auto* self = reinterpret_cast<Graph_node_block*>(block.get());
    if (block->token.is_cancel_requested())
    {
        block->cancel();   // skip body; on_complete drains the run
        return;
    }

    block->num_locks.store(Block::execution_flag + 1, std::memory_order_relaxed);
    auto prev = std::move(detail::current_task);
    detail::current_task = block;

    self->graph->nodes_[self->index].run();   // node body: installs its own Access_scope

    detail::current_task = std::move(prev);

    // Drop the self-lock; if no nested tasks are pending, complete now (fires on_complete);
    // otherwise the last nested task completes us.
    if (block->num_locks.fetch_sub(1, std::memory_order_acq_rel) == Block::execution_flag + 1)
        block->complete();
}

// The node block's `on_complete`: the node's body and all its nested tasks have settled.
void Static_task_graph::graph_node_completed(detail::Task_control_block* block)
{
    auto* self = reinterpret_cast<Graph_node_block*>(block);
    node_complete(*self->graph->run_, self->index);
}

void Static_task_graph::node_complete(Run_state& run, int index)
{
    Node& node = run.graph->nodes_[index];

    // Early release: free each object this node was the last to touch, so queued async
    // on it can run. Safe -- the count hits 0 only after every node accessing that
    // object has completed (this runs post-completion, so after any nested sub-work).
    for (int pi : node.pipe_indices)
        if (run.remaining_accessors[pi].fetch_sub(1, std::memory_order_acq_rel) == 1)
            detail::pipe_release(*run.scheduler, *run.graph->distinct_pipes_[pi]);

    for (int successor : node.successors)
        if (run.remaining_deps[successor].fetch_sub(1, std::memory_order_acq_rel) == 1)
            on_data_ready(run, successor);

    if (run.remaining_nodes.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        // Keep `done` alive across the settle: completing it notifies its cv, which can
        // wake a waiter (`execute().get()`) that immediately starts the next run --
        // overwriting `run.done` and dropping its own handle -- destroying this block
        // mid-notify. The local ref holds it until settle returns. (The old per-run
        // Run_state kept it alive via the worker closures; the reused slot does not.)
        std::shared_ptr<detail::Task_control_block> done = run.done;
        if (run.token.is_cancel_requested())
            done->cancel();
        else
            done->complete();
    }
}

Task<void> Static_task_graph::execute(Scheduler& scheduler, Cancellation_token token)
{
    if (!compiled_)
        ts::fatal("Static_task_graph::execute called before compile()");

    // Reuse the run state built at compile() (one run at a time; a full get() barrier
    // between runs guarantees the previous run is quiescent -- see docs §7.1). Only the
    // completion handle is freshly allocated, since a prior run's handle may still be
    // held by the caller.
    Run_state& run = *run_;
    run.graph = this;   // refresh: a moved graph's back pointers point at the moved-from object
    run.scheduler = &scheduler;
    run.token = token;
    run.done = std::make_shared<detail::Task_control_block>();

    for (size_t i = 0; i < nodes_.size(); ++i)
    {
        run.remaining_deps[i].store(nodes_[i].indegree, std::memory_order_relaxed);
        run.remaining_objects[i].store(static_cast<int>(nodes_[i].pipe_indices.size()), std::memory_order_relaxed);
        run.launched[i].store(0, std::memory_order_relaxed);

        // Re-arm the node's task block for this run (its successors/prerequisites/
        // continuations are never populated -- graph edges use remaining_deps, completion
        // uses on_complete -- so nothing there needs clearing).
        auto* w = reinterpret_cast<Graph_node_block*>(nodes_[i].block.get());
        w->graph = this;   // refresh back pointer too (see above)
        detail::Task_control_block& b = w->core;
        b.started.store(false, std::memory_order_relaxed);
        b.completed = false;
        b.cancelled = false;
        b.ready.store(false, std::memory_order_relaxed);
        b.num_locks.store(0, std::memory_order_relaxed);
        b.token = token;
    }
    for (size_t i = 0; i < distinct_pipes_.size(); ++i)
    {
        run.remaining_accessors[i].store(static_cast<int>(pipe_accessors_[i].size()), std::memory_order_relaxed);
        run.object_initiated[i].store(0, std::memory_order_relaxed);
    }
    run.remaining_nodes.store(static_cast<int>(nodes_.size()), std::memory_order_relaxed);

    Task<void> result(run.done);

    if (nodes_.empty())
    {
        run.done->complete();
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
