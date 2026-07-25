#include "ts/static_task_graph.h"
#include "ts/fatal.h"
// The tools seam, one include: `Trace_stamps` + the structure consumers. All
// `TS_PROFILING` gating happens inside it.
#include "graph_introspect.h"

#include <atomic>
#include <deque>
#include <map>
#include <set>

namespace ts
{

struct Static_task_graph::Run_state
{
    explicit Run_state(size_t node_count)
        : remaining_deps(node_count)
        , preheld(node_count)
        , stamps(node_count)
    {}

    Static_task_graph* graph = nullptr;
    Scheduler* scheduler = nullptr;
    Cancellation_token token;
    std::vector<std::atomic<int>> remaining_deps;        // per node: unmet data prerequisites
    std::vector<std::atomic<std::uint64_t>> preheld;     // per node: bitmask of pipe_indices positions handed from a predecessor (skip acquire)
    std::atomic<int> remaining_nodes{ 0 };
    detail::Task_ptr done;
    tools::Trace_stamps stamps;
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

Graph_node& Graph_node::priority(Priority p)
{
    if (graph_)
        graph_->nodes_[index_].priority = p;   // applied to the block in execute() (see re-arm)
    return *this;
}

Graph_node& Graph_node::set_inline()
{
    if (graph_)
        graph_->nodes_[index_].inline_dispatch = true;   // applied to the block in execute() (see re-arm)
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

void Static_task_graph::compile(const char* DOT_path)
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

    // Distinct set of pipes the graph touches; a pipe's index in this vector is its
    // canonical id (nodes acquire in ascending-index order -> deadlock-free multi-acquire).
    std::set<detail::Pipe*> pipes;
    for (const Node& node : nodes_)
        for (detail::Pipe* p : node.pipes)
            pipes.insert(p);
    distinct_pipes_.assign(pipes.begin(), pipes.end());

    std::map<detail::Pipe*, int> index_of;
    for (int i = 0; i < static_cast<int>(distinct_pipes_.size()); ++i)
        index_of[distinct_pipes_[i]] = i;

    // Per node: the deduped pipe indices it touches (ascending = canonical acquire order)
    // and its effective access mode per pipe (write wins if it lists one both ways). Drives
    // the per-node mode-aware acquire/release (on_data_ready / node_complete).
    for (int n = 0; n < static_cast<int>(nodes_.size()); ++n)
    {
        std::map<int, Access> mode_by_pipe;   // ordered by pipe index -> canonical
        for (size_t k = 0; k < nodes_[n].pipes.size(); ++k)
        {
            int pi = index_of[nodes_[n].pipes[k]];
            Access m = nodes_[n].access[k].second;
            auto [it, inserted] = mode_by_pipe.try_emplace(pi, m);
            if (!inserted && m == Access::read_write)
                it->second = Access::read_write;
        }
        nodes_[n].pipe_indices.clear();
        nodes_[n].pipe_modes.clear();
        for (const auto& [pi, m] : mode_by_pipe)
        {
            nodes_[n].pipe_indices.push_back(pi);
            nodes_[n].pipe_modes.push_back(m);
        }
    }

    detect_cycles();

    // Each node runs as a reusable task block, allocated once here and re-armed per run
    // (§7.1) -- so a run dispatches every node without allocating. `execute`/`on_complete`
    // are fixed fn-ptrs; graph+index let those hooks reach the node body and the run.
    for (int i = 0; i < static_cast<int>(nodes_.size()); ++i)
    {
        auto* wrapper = new Graph_node_block();
        wrapper->core.destroy = [](detail::Task_control_block* c) { delete reinterpret_cast<Graph_node_block*>(c); };
        wrapper->graph = this;
        wrapper->index = i;
        wrapper->core.execute = &run_graph_node;
        wrapper->core.on_complete = &graph_node_completed;
        nodes_[i].block = detail::Task_ptr(&wrapper->core);
    }

    // Reused per-run state: values are reset each execute(), vector capacity persists, so
    // a run allocates only its completion handle (`done`). Rebuilt here since node/pipe
    // counts can change between compiles.
    run_ = std::make_unique<Run_state>(nodes_.size());
    run_->graph = this;

    compiled_ = true;

    // Export the compiled structure to the tooling consumers (tools/graph_introspect.h):
    // the DOT dump when a path was given, and a re-arm of the attached trace (its
    // aggregates reset). No-op arguments are handled inside; compiles to nothing under
    // TS_PROFILING=0.
    tools::export_structure(nodes_, explicit_edges_, DOT_path, trace_);
}

void Static_task_graph::set_trace(tools::Graph_trace* trace)
{
    if (trace && !compiled_)
        ts::fatal("Static_task_graph::set_trace requires a compiled graph (call compile() first)");
    trace_ = trace;
    tools::export_structure(nodes_, explicit_edges_, /*DOT_path*/ nullptr, trace_);
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

// A node has become data-ready (all data prerequisites met). Acquire the objects it
// touches, in canonical order, holding each -- then run. Called exactly once per node (the
// single remaining_deps 0-transition, or a root at kickoff), so acquisition starts once.
void Static_task_graph::on_data_ready(Run_state& run, int index)
{
    run.stamps.mark_ready(index);
    acquire_next(run, index, 0, /*synchronous*/ true);
}

// Acquire the node's `pos`-th object (in ascending pipe-index / canonical order), mode-aware
// and holding it; when it is held, advance to the next; when all are held, run the node.
// Sequential + canonical order makes multi-object acquire deadlock-free (a node never holds
// a higher-index object while waiting on a lower one). Immediate acquisitions recurse here
// synchronously (bounded by the node's object count); a contended one defers to `pipe_acquire`'s
// callback (fires on a worker when the object frees). Edges guarantee no other NODE contends
// the same object concurrently, so the only wait is on out-of-band async.
void Static_task_graph::acquire_next(Run_state& run, int index, int pos, bool synchronous)
{
    const Node& node = run.graph->nodes_[index];
    if (pos >= static_cast<int>(node.pipe_indices.size()))
    {
        // All objects held. An inline node whose acquires ALL succeeded synchronously (still
        // on the settling thread) runs here via the shared trampoline (its block has
        // `run_inline` set, so `dispatch_ready` takes the inline path -- bounded, iterative);
        // otherwise (queued node, or an acquire deferred to a worker) go through the queue.
        // Reading the node's generation here is race-free: a node dispatches exactly once
        // per run, runs are sequential, and re-arm happens only at the NEXT `execute()` --
        // after every dispatch of this run has been consumed (the run's `done` gates on all
        // node completions). No dispatch can coexist with a re-arm, unlike the reusable-
        // builder path that needed release-time generation capture (see task.h `release`).
        if (synchronous && node.inline_dispatch)
            detail::Task_control_block::dispatch_ready(node.block, node.block->generation());
        else
            run_node(run, index);
        return;
    }

    // Pre-held: a predecessor handed us this object (skipped its release; the pipe is already
    // held in the right mode). No pipe op -- treat as acquired, stay on the settling thread.
    if (pos < 64 && (run.preheld[index].load(std::memory_order_relaxed) >> pos) & 1u)
    {
        acquire_next(run, index, pos + 1, synchronous);
        return;
    }

    int pi = node.pipe_indices[pos];
    Access mode = node.pipe_modes[pos];
    Run_state* rp = &run;   // stable (run_ outlives the run)
    bool acquired = detail::pipe_acquire(*run.scheduler, *run.graph->distinct_pipes_[pi], mode,
        [rp, index, pos] { acquire_next(*rp, index, pos + 1, /*synchronous*/ false); });

    if (acquired)
        acquire_next(run, index, pos + 1, synchronous);   // still on the settling thread
}

// Dispatch a node: submit its (re-armed) block via the raw scheduler API -- a fn-ptr
// plus the Node address, so no per-node closure is allocated.
void Static_task_graph::run_node(Run_state& run, int index)
{
    Node& node = run.graph->nodes_[index];
    run.scheduler->submit(&node_trampoline, &node, node.block->flags.priority);
}

// Raw scheduler entry: run the node's block. Kept alive by the graph (Node owns the
// block); the trampoline holds no ownership. The `generation()` read at run time is
// race-free for graph nodes (sequential runs; every dispatch consumed before the run's
// `done` settles; re-arm only at the next `execute()` -- see `acquire_next`).
void Static_task_graph::node_trampoline(void* node)
{
    auto* n = static_cast<Node*>(node);
    n->block->execute(n->block, n->block->generation());
}

// The node block's `execute`. Mirrors `Executable::run`, but reaches the body via
// graph+index (no stored body) and completes via the block's `on_complete` hook: it
// installs `current_task` + the execution-flag self-lock so the body may spawn NESTED
// tasks (`ts::nested`) that gate the node's completion, then completes once the self-lock
// and all nested tasks release. The block carries the run's `token`, so a cancelled node
// skips its body (settling cancelled) -- `on_complete` still fires, keeping the drain
// going.
void Static_task_graph::run_graph_node(const detail::Task_ptr& block, std::uint64_t gen)
{
    using Block = detail::Task_control_block;

    if (!block->claim(gen))
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

    self->graph->run_->stamps.mark_start(self->index);

    {
        // Own this node for trace attribution: its body's busy (and, via inheritance, its
        // slices' and async work's) counts toward this node's true-busy. No-op untraced.
        detail::Trace_owner_scope trace_owner_scope(self->index);
        detail::Trace_busy_scope trace_busy_scope;
        self->graph->nodes_[self->index].run();   // node body: installs its own Access_scope
    }

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

// The ready successor to hand object `pi` to (see the header): exactly one ready successor
// accesses `pi`, and in mode `m`.
int Static_task_graph::handoff_target(Run_state& run, const std::vector<int>& ready, int pi, Access m)
{
    int found = -1;
    Access found_mode = Access::read_only;
    for (int s : ready)
    {
        const Node& sn = run.graph->nodes_[s];
        for (size_t k = 0; k < sn.pipe_indices.size(); ++k)
            if (sn.pipe_indices[k] == pi)
            {
                if (found != -1)
                    return -1;   // a second ready accessor -> not a clean single handoff
                found = s;
                found_mode = sn.pipe_modes[k];
                break;
            }
    }
    return (found != -1 && found_mode == m) ? found : -1;
}

// Mark object `pi` as pre-held for `node_index` (its `acquire_next` will skip it).
void Static_task_graph::mark_preheld(Run_state& run, int node_index, int pi)
{
    const Node& n = run.graph->nodes_[node_index];
    for (size_t pos = 0; pos < n.pipe_indices.size(); ++pos)
        if (n.pipe_indices[pos] == pi)
        {
            if (pos < 64)
                run.preheld[node_index].fetch_or(std::uint64_t{ 1 } << pos, std::memory_order_relaxed);
            return;
        }
}

void Static_task_graph::node_complete(Run_state& run, int index)
{
    Node& node = run.graph->nodes_[index];

    run.stamps.mark_end(index);

    // Phase 1: settle successor data-deps; collect those this node's completion makes ready
    // (it is exclusively their trigger, so this node's thread owns them until we hand off /
    // dispatch them below -- no race with another prerequisite).
    std::vector<int>& ready = node.ready_buf;
    ready.clear();
    for (int successor : node.successors)
        if (run.remaining_deps[successor].fetch_sub(1, std::memory_order_acq_rel) == 1)
            ready.push_back(successor);

    // Phase 2: hand off or release each object this node held. HANDOFF (skip release + skip
    // the successor's re-acquire) when exactly one ready successor takes the object in the
    // SAME mode -- the pipe state is then already correct for it, so a release + re-acquire
    // round-trip is pure waste. The handed object stays held across the edge (no gap), which
    // is fine: the successor runs immediately (it just went ready). Otherwise RELEASE (freeing
    // the object for async / a later node -- the gap-freeing). Runs post-completion
    // (after any nested sub-work).
    for (size_t k = 0; k < node.pipe_indices.size(); ++k)
    {
        int pi = node.pipe_indices[k];
        Access m = node.pipe_modes[k];
        int target = handoff_target(run, ready, pi, m);
        if (target >= 0)
            mark_preheld(run, target, pi);   // hand it directly -> no pipe op
        else
            detail::pipe_release(*run.scheduler, *run.graph->distinct_pipes_[pi], m);
    }

    // Phase 3: trigger the ready successors (they acquire their objects, skipping any handed
    // to them).
    for (int successor : ready)
        on_data_ready(run, successor);

    if (run.remaining_nodes.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        // Keep `done` alive across the settle: completing it notifies its cv, which can
        // wake a waiter (`execute().sync()`) that immediately starts the next run --
        // overwriting `run.done` and dropping its own handle -- destroying this block
        // mid-notify. The local ref holds it until settle returns.
        detail::Task_ptr done = run.done;
        // Fold this run's stamps into the trace BEFORE the completion handle settles (a
        // sync()ed caller then reads a consistent trace).
        run.stamps.fold(run.graph->trace_, run.token.is_cancel_requested());
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
    run.done = detail::make_bare_block();
    run.stamps.begin_run(trace_, scheduler);

    for (size_t i = 0; i < nodes_.size(); ++i)
    {
        run.remaining_deps[i].store(nodes_[i].indegree, std::memory_order_relaxed);
        run.preheld[i].store(0, std::memory_order_relaxed);   // no handoffs yet this run

        // Re-arm the node's task block for this run (its successors/prerequisites/
        // continuations are never populated -- graph edges use remaining_deps, completion
        // uses on_complete -- so nothing there needs clearing).
        auto* w = reinterpret_cast<Graph_node_block*>(nodes_[i].block.get());
        w->graph = this;   // refresh back pointer too (see above)
        detail::Task_control_block& b = w->core;
        b.run_state.store(0, std::memory_order_relaxed);   // generation 0, unclaimed (nodes aren't reset())
        b.completed = false;
        b.cancelled = false;
        b.prereq_cancelled.store(false, std::memory_order_relaxed);
        b.ready.store(false, std::memory_order_relaxed);
        b.num_locks.store(0, std::memory_order_relaxed);
        b.token = token;
        b.flags.priority = nodes_[i].priority;          // pick up any Graph_node::priority set since last run
        b.flags.run_inline = nodes_[i].inline_dispatch; // so dispatch_ready takes the inline path for an inline node
    }
    run.remaining_nodes.store(static_cast<int>(nodes_.size()), std::memory_order_relaxed);

    Task<void> result(run.done);

    if (nodes_.empty())
    {
        run.done->complete();
        return result;
    }

    // Objects are acquired per node (see on_data_ready / acquire_next), mode-aware and only
    // over each accessor's [acquire, complete] window -- so a graph object is free in the
    // gaps (no whole-run reservation). Kick off every root (indegree 0).
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].indegree == 0)
            on_data_ready(run, static_cast<int>(i));

    return result;
}

} // namespace ts
