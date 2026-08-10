#include "ts/static_task_graph.h"
#include "ts/fatal.h"
// The tools seam, one include: `Trace_stamps` + the structure consumers. All
// `TS_PROFILING` gating happens inside it.
#include "graph_introspect.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <map>
#include <new>
#include <set>

namespace ts
{

struct Static_task_graph::Run_state
{
    explicit Run_state(size_t node_count)
        : remaining_deps(node_count)
        , stamps(node_count)
    {}

    Static_task_graph* graph = nullptr;
    Scheduler* scheduler = nullptr;
    Cancellation_token token;
    std::vector<std::atomic<int>> remaining_deps;        // per node: unmet data prerequisites
    std::atomic<int> remaining_nodes{ 0 };
    detail::Task_ptr done;
    tools::Trace_stamps stamps;
};

namespace
{

// A graph node's reusable task block: the monomorphic control block (first member, so a
// `Task_control_block*` aliases / `reinterpret_cast`s back to it) plus the back-pointers
// its execute/on_complete hooks need to reach the node body and the run. Allocated once
// in compile(), re-armed each execute() - so a run dispatches nodes with no per-node
// allocation. The body is not stored here (reached via graph->nodes_[index].run), so
// there is no per-run body closure either.
struct Graph_node_block
{
    detail::Task_control_block core;   // must be first
    Static_task_graph* graph = nullptr;
    int index = -1;
};

} // namespace

// Defined here, where Run_state is complete (the header's `run_` unique_ptr needs it).
Static_task_graph::Static_task_graph() = default;
Static_task_graph::Static_task_graph(Static_task_graph&&) noexcept = default;

#if TS_SAFETY_CHECKS
void Static_task_graph::check_quiescent_and_release_pipes(const char* where) noexcept
{
    if (run_ && run_->remaining_nodes.load(std::memory_order_acquire) != 0)
    {
        char msg[160];
        std::snprintf(msg, sizeof msg,
            "Static_task_graph %s while a run is in flight (sync() the run's completion first)",
            where);
        ts::fatal(msg);
    }
    // A moved-from graph's vector is empty (the registrations rode the move); a
    // never-compiled graph's likewise.
    for (detail::Pipe* p : distinct_pipes_)
        p->graph_refs.fetch_sub(1, std::memory_order_release);
}
#endif

Static_task_graph::~Static_task_graph()
{
#if TS_SAFETY_CHECKS
    check_quiescent_and_release_pipes("destroyed");
#endif
}

Static_task_graph& Static_task_graph::operator=(Static_task_graph&& other) noexcept
{
    if (this == &other)
        return *this;
    // Destroy-and-move-construct in place: the destructor carries the quiescence check
    // and pipe deregistration for the overwritten graph, and the member-wise move stays
    // maintenance-free (no hand-listed member moves to drift out of sync). Exceptions
    // are disabled project-wide, so the reconstruct cannot be interrupted.
    this->~Static_task_graph();
    new (this) Static_task_graph(std::move(other));
    return *this;
}

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
    {
        for (const auto& [instance_b, mode_b] : b.access)
        {
            if (instance_a == instance_b && (mode_a == Access::read_write || mode_b == Access::read_write))
                return true;
        }
    }
    return false;
}

void Static_task_graph::compile(const char* DOT_path)
{
#if TS_SAFETY_CHECKS
    // Recompiling releases the previous compile's pipe registrations (the new set
    // re-registers below); also fatals on a recompile while a run is in flight.
    check_quiescent_and_release_pipes("recompiled");
#endif

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
    {
        for (int j = i + 1; j < static_cast<int>(nodes_.size()); ++j)
        {
            if (conflicts(nodes_[i], nodes_[j]))
                edges.insert({ i, j });
        }
    }

    for (const auto& [from, to] : edges)
    {
        nodes_[from].successors.push_back(to);
        ++nodes_[to].indegree;
    }

    // Distinct set of pipes the graph touches; a pipe's index in this vector is its
    // canonical id (nodes acquire in ascending-index order -> deadlock-free multi-acquire).
    std::set<detail::Pipe*> pipes;
    for (const Node& node : nodes_)
    {
        for (detail::Pipe* p : node.pipes)
            pipes.insert(p);
    }
    distinct_pipes_.assign(pipes.begin(), pipes.end());

#if TS_SAFETY_CHECKS
    // Register this graph on every pipe it references; `~Guarded` fatals while any
    // compiled graph still holds its pipe (see `Pipe::graph_refs`). Balanced by
    // `check_quiescent_and_release_pipes` (dtor / recompile / move-assign overwrite).
    for (detail::Pipe* p : distinct_pipes_)
        p->graph_refs.fetch_add(1, std::memory_order_relaxed);
#endif

    std::map<detail::Pipe*, int> index_of;
    for (int i = 0; i < static_cast<int>(distinct_pipes_.size()); ++i)
        index_of[distinct_pipes_[i]] = i;

    // Per distinct pipe: the guarded instance behind it and the strongest mode any node uses
    // on it. Both feed the nested-run lend decision (`bind_links_for_run`), which asks the
    // caller's `Access_context` - keyed by instance address, not by pipe - whether its
    // grant covers what this graph needs. A node's `pipes[k]` and `access[k]` are parallel.
    pipe_instances_.assign(distinct_pipes_.size(), nullptr);
    pipe_modes_.assign(distinct_pipes_.size(), Access::read_only);
    for (const Node& node : nodes_)
    {
        for (std::size_t k = 0; k < node.pipes.size(); ++k)
        {
            int pi = index_of[node.pipes[k]];
            pipe_instances_[pi] = node.access[k].first;
            if (node.access[k].second == Access::read_write)
                pipe_modes_[pi] = Access::read_write;
        }
    }
    lent_.assign(distinct_pipes_.size(), 0);
    links_lent_ = false;

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
    // (§7.1) - so a run dispatches every node without allocating. `execute`/`on_complete`
    // are fixed fn-ptrs; graph+index let those hooks reach the node body and the run.
    for (int i = 0; i < static_cast<int>(nodes_.size()); ++i)
    {
        auto* wrapper = new Graph_node_block();
        wrapper->core.destroy = [](detail::Task_control_block* c) { delete reinterpret_cast<Graph_node_block*>(c); };
        wrapper->graph = this;
        wrapper->index = i;
        wrapper->core.execute = &run_graph_node;
        wrapper->core.on_complete = &graph_node_completed;
        // The node block is owned by this graph (its `Task_ptr` below) for the whole run, so
        // its queued dispatch borrows a raw pointer - no dispatch-hop refcount (Opt 2). Set
        // once here; the per-run re-arm rewrites priority/run_inline but preserves this bit.
        wrapper->core.flags.borrowed = true;
        nodes_[i].block = detail::Task_ptr(&wrapper->core);
        // The node's block carries the node's identity, so every diagnostic that names a
        // task names the node - including the pipe entries it takes, which are this block.
        detail::set_task_name(nodes_[i].block, nodes_[i].name);
    }

    // Bind every node's pipe links into one slab (tail pipe): the node's `pipe_indices`
    // are ascending over the address-sorted `distinct_pipes_`, so link order is canonical.
    // Bound once here, re-armed each execute(); the blocks' `pipe_links` point into the
    // slab (stable across a graph move - the unique_ptr's buffer moves by ownership).
    {
        std::size_t total = 0;
        for (const Node& n : nodes_)
            total += n.pipe_indices.size();
        node_links_ = std::make_unique<detail::Pipe_link[]>(total);
        std::size_t offset = 0;
        for (Node& n : nodes_)
        {
            n.block->pipe_links = (n.pipe_indices.empty()) ? nullptr : &node_links_[offset];
            for (std::size_t k = 0; k < n.pipe_indices.size(); ++k)
            {
                detail::Pipe_link& l = node_links_[offset + k];
                l.owner = n.block.get();
                l.pipe = distinct_pipes_[n.pipe_indices[k]];
                l.index = static_cast<std::uint8_t>(k);
                l.mode = n.pipe_modes[k];
            }
            n.block->pipe_count = static_cast<std::uint8_t>(n.pipe_indices.size());
            offset += n.pipe_indices.size();
        }
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
    {
        if (indegree[i] == 0)
            ready.push_back(static_cast<int>(i));
    }

    size_t visited = 0;
    while (!ready.empty())
    {
        int n = ready.front();
        ready.pop_front();
        ++visited;
        for (int s : nodes_[n].successors)
        {
            if (--indegree[s] == 0)
                ready.push_back(s);
        }
    }

    if (visited != nodes_.size())
        ts::fatal("Static_task_graph has a cycle");
}

// A node has become data-ready (all data prerequisites met). Called exactly once per node
// (the single remaining_deps 0-transition, or a root at kickoff).
// Tail pipe: the node's pipe turns are its remaining `num_locks` (seeded to `pipe_count`
// at re-arm); entering line 0 starts the sequential canonical cascade, and the last turn's
// release dispatches the node through the standard `dispatch_ready` (queue at the node's
// priority, or the inline trampoline for a `set_inline` node). An object-free node
// dispatches directly.
void Static_task_graph::on_data_ready(Run_state& run, int index)
{
    run.stamps.mark_ready(index);
    Node& node = run.graph->nodes_[index];
    detail::Task_control_block* blk = node.block.get();
    if (blk->pipe_count == 0)
    {
        // Object-free node: dispatch directly. An inline node keeps the shared inline
        // trampoline; a queued node goes to the run's cached scheduler (Opt 1) rather than
        // re-resolving `global_scheduler()` per node inside `submit_ready`.
        if (blk->flags.run_inline)
            detail::Task_control_block::dispatch_ready(node.block);
        else
            detail::submit_borrowed_on(*run.scheduler, blk);
    }
    else
    {
        detail::pipe_enter_first(blk);
    }
}


// The node block's `execute`. Mirrors `Executable::run`, but reaches the body via
// graph+index (no stored body) and completes via the block's `on_complete` hook: it
// installs `current_task` + the execution-flag self-lock so a coroutine-node body's frame
// (or a nested graph run) can gate the node's completion via `detail::add_nested`, then
// completes once the self-lock and any nested completions release. The block carries the
// run's `token`, so a cancelled node skips its body (settling cancelled) - `on_complete`
// still fires, keeping the drain going.
void Static_task_graph::run_graph_node(const detail::Task_ptr& block)
{
    using Block = detail::Task_control_block;

    if (!block->claim())
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
    // The settle-must-advance-links contract (the graph is the second pipe-task creation
    // site next to `make_piped_executable`): retire this node's line entries first, so a
    // successor going data-ready below enters lines the node no longer holds. An object-free
    // node entered no pipes (`pipe_count == 0`, its data-ready path dispatched directly), so
    // it has nothing to advance - skip the cross-TU call entirely (Opt 3).
    if (block->pipe_count != 0)
        detail::advance_pipe_links(block);
    node_complete(*self->graph->run_, self->index);
}


void Static_task_graph::node_complete(Run_state& run, int index)
{
    Node& node = run.graph->nodes_[index];

    run.stamps.mark_end(index);

    // Phase 1: settle successor data-deps; collect those this node's completion makes ready
    // (it is exclusively their trigger, so this node's thread owns them until we hand off /
    // dispatch them below - no race with another prerequisite).
    std::vector<int>& ready = node.ready_buf;
    ready.clear();
    for (int successor : node.successors)
    {
        if (run.remaining_deps[successor].fetch_sub(1, std::memory_order_acq_rel) == 1)
            ready.push_back(successor);
    }


    // Phase 3: trigger the ready successors (they acquire their objects, skipping any handed
    // to them).
    for (int successor : ready)
        on_data_ready(run, successor);

    if (run.remaining_nodes.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        // Keep `done` alive across the settle: completing it notifies its cv, which can
        // wake a waiter (`execute().sync()`) that immediately starts the next run --
        // overwriting `run.done` and dropping its own handle - destroying this block
        // mid-notify. The local ref holds it until settle returns.
        detail::Task_ptr done = run.done;
        // Fold this run's stamps into the trace before the completion handle settles (a
        // sync()ed caller then reads a consistent trace). A worker-less run is skipped:
        // its per-node starts are cumulative serial offsets, dispatch-wait measures the
        // trampoline, the critical path degenerates to the whole chain, and utilization
        // has no workers to measure - folding it would corrupt the parallel profile. (A
        // separate serial-baseline lane - clean per-node total_work + serial-vs-parallel
        // contention deltas - is a planned trace feature, not a fold into these
        // aggregates.)
        run.stamps.fold(run.graph->trace_,
            run.token.is_cancel_requested() || run.scheduler->single_threaded());
        if (run.token.is_cancel_requested())
            done->cancel();
        else
            done->complete();
    }
}

// Resolve the lend set for the run about to start and bind the node link slab accordingly.
//
// A lent object is one the calling task already holds a covering grant on: the inner nodes
// then skip their pipe turns on it entirely. That is not an optimization - taking the turn
// would queue the inner node behind the very grant its caller holds, and the caller is
// suspended waiting for the inner run, so the run would never finish. Skipping is safe
// because the caller's grant is what excludes everyone else for the whole nested run (an
// awaited inner run is strictly contained in the caller's grant window), and the compiled
// conflict edges still order the inner nodes among themselves on that object.
//
// Binding is the whole mechanism: a link that is not bound is a turn that is never taken.
// The slab keeps its per-node offsets, so the surviving links are packed at the front of
// each node's range with sequential `index` values - the cascade's only requirement.
void Static_task_graph::bind_links_for_run(bool detach)
{
    // A detached run structurally receives no lend (and no inherited context): it may outlive
    // the launcher, so the containment argument above does not hold. It simply queues behind
    // the caller's holds like any external work.
    const Access_context* ctx = detach ? nullptr : detail::current_access;
    bool any = false;
    for (std::size_t i = 0; i < distinct_pipes_.size(); ++i)
    {
        char lend = 0;
        if (ctx != nullptr && pipe_instances_[i] != nullptr)
        {
            if (ctx->grants(pipe_instances_[i], pipe_modes_[i]))
            {
                lend = 1;
            }
#if TS_SAFETY_CHECKS
            else if (pipe_modes_[i] == Access::read_write && ctx->grants(pipe_instances_[i], Access::read_only))
            {
                // Mode-incompatible overlap: the caller holds a read grant on an object this
                // graph writes. A read grant cannot cover a write, and upgrading would mean
                // re-acquiring the pipe - behind the caller's own read hold, which the
                // caller cannot release while it waits. Fatal at the cause.
                ts::fatal("Static_task_graph::execute - the calling task holds READ access on an object this "
                          "graph writes; a read grant cannot be lent to a writer (declare the write on the "
                          "calling node, or move the writing node out of the nested graph)");
            }
#endif
        }
        lent_[i] = lend;
        any = any || lend != 0;
    }

#if TS_SAFETY_CHECKS
    if (any && detail::current_scope_children != nullptr)
    {
        // Lending hands the caller's exclusivity to the inner run, but a still-running earlier
        // nested run is also executing under that same grant - so both could touch the lent
        // object at once, each "validly". Conservative rule: no un-awaited nested run may be in
        // flight when another lends. Settled runs are not a hazard, and the scope list only drops
        // them at completion, so filter on `ready` rather than on emptiness (else a fire-and-settle
        // run earlier in the body would fatal).
        for (const detail::Task_ptr& child : *detail::current_scope_children)
        {
            if (!child->ready.load(std::memory_order_acquire))
            {
                ts::fatal("Static_task_graph::execute - lending an object to a nested run while an earlier "
                          "un-awaited nested run of the calling task is still in flight (co_await the previous "
                          "nested run before starting another)");
            }
        }
    }
#endif

    if (!any && !links_lent_)
        return;   // the common case: full binding already in place, nothing to rebind

    std::size_t offset = 0;
    for (Node& node : nodes_)
    {
        std::size_t bound = 0;
        for (std::size_t k = 0; k < node.pipe_indices.size(); ++k)
        {
            if (any && lent_[node.pipe_indices[k]] != 0)
                continue;
            detail::Pipe_link& link = node_links_[offset + bound];
            link.owner = node.block.get();
            link.pipe = distinct_pipes_[node.pipe_indices[k]];
            link.index = static_cast<std::uint8_t>(bound);
            link.mode = node.pipe_modes[k];
            ++bound;
        }
        node.block->pipe_links = (bound == 0) ? nullptr : &node_links_[offset];
        node.block->pipe_count = static_cast<std::uint8_t>(bound);
        offset += node.pipe_indices.size();
    }
    links_lent_ = any;
}

Task<void> Static_task_graph::execute(Execution_options opts)
{
    Cancellation_token token = std::move(opts.token);
    if (!compiled_)
        ts::fatal("Static_task_graph::execute called before compile()");

#if TS_SAFETY_CHECKS
    // One run at a time. Previously unguarded (only the destructor checked quiescence), which
    // made a second concurrent `execute()` corrupt the shared `Run_state` silently. It became
    // a plausible mistake rather than pure misuse once nested runs landed: a pre-compiled
    // sub-graph invoked from two concurrently running parents collides here. v1 rule is one
    // instance per concurrent user (compile a clone, or order the parents with an edge);
    // run-queueing is TODO 2.3.
    if (run_ && run_->remaining_nodes.load(std::memory_order_acquire) != 0)
    {
        ts::fatal("Static_task_graph::execute called while a run of this graph is still in flight "
                  "(one run at a time - await the previous run, or use a separate graph instance)");
    }
#endif

    // Runs on the one global scheduler (whatever a `Scheduler_scope` currently has it
    // configured as).
    Scheduler& scheduler = global_scheduler();

    // Reuse the run state built at compile() (one run at a time; a full get() barrier
    // between runs guarantees the previous run is quiescent - see docs §7.1). Only the
    // completion handle is freshly allocated, since a prior run's handle may still be
    // held by the caller.
    Run_state& run = *run_;
    run.graph = this;   // refresh: a moved graph's back pointers point at the moved-from object
    run.scheduler = &scheduler;
    run.token = token;
    run.done = detail::make_bare_block();

    // Arm the trace and snapshot the machinery/body counters before the per-run setup below,
    // so the setup span the `Trace_setup_scope` books folds into this run's machinery delta.
    run.stamps.begin_run(trace_, scheduler);

#if TS_PROFILING
    // Fold the per-run setup + initial dispatch (link binding, node re-arm, indegree init,
    // root dispatch) into machinery M - that work runs here on the calling thread in no
    // `run_task` span, scales with node count, and previously escaped the overhead metric
    // entirely. Function-scope, so it also covers the early empty-graph return. In worker-less
    // mode the initial dispatch drains the whole frame inline within this span; the scope flags
    // it `run_task`-booked, so each inline body nets its own span back out, leaving pure
    // machinery. Armed-only; a no-op on an untraced run.
    detail::Trace_setup_scope setup_cost;
#endif

    // Resolve lending and bind the links before the per-node re-arm below, which seeds each
    // block's lock counter from its (possibly reduced) `pipe_count`.
    bind_links_for_run(opts.detach);

    for (size_t i = 0; i < nodes_.size(); ++i)
    {
        run.remaining_deps[i].store(nodes_[i].indegree, std::memory_order_relaxed);

        // Re-arm the node's task block for this run (its successors/prerequisites/
        // continuations are never populated - graph edges use remaining_deps, completion
        // uses on_complete - so nothing there needs clearing).
        auto* w = reinterpret_cast<Graph_node_block*>(nodes_[i].block.get());
        w->graph = this;   // refresh back pointer too (see above)
        detail::Task_control_block& b = w->core;
        b.body_claimed.store(false, std::memory_order_relaxed);   // unclaimed (nodes aren't reset())
        b.completed = false;
        b.cancelled = false;
        b.prereq_cancelled.store(false, std::memory_order_relaxed);
        b.ready.store(false, std::memory_order_relaxed);
        // The node's pipe turns are its pre-execution locks (entered at data-ready): seed
        // the counter with the link count. The links themselves need no re-arm - their
        // bind-time fields are stable, and `next` is queue state owned by the pipe mutex
        // (an entry leaves the queue before its turn fires, every run).
        b.num_locks.store(b.pipe_count, std::memory_order_relaxed);
        b.pipes_entered = 0;
        b.token = token;
        b.flags.priority = nodes_[i].priority;          // pick up any Graph_node::priority set since last run
        b.flags.run_inline = nodes_[i].inline_dispatch; // so dispatch_ready takes the inline path for an inline node
    }
    run.remaining_nodes.store(static_cast<int>(nodes_.size()), std::memory_order_relaxed);

    Task<void> result(run.done);

    // A nested run joins the calling task's implicit scope by default, so an un-awaited inner
    // run cannot float past its launcher: the caller (a node, a coroutine frame) completes --
    // and a node releases its objects - only after the run settles. Attached before kickoff,
    // so the run cannot settle in the window. `{.detach = true}` opts out, and a run started
    // outside any task has nothing to join.
    if (!opts.detach && detail::current_task)
        detail::add_nested(run.done);

    if (nodes_.empty())
    {
        run.done->complete();
        return result;
    }

    // Objects are acquired per node (see on_data_ready / acquire_next), mode-aware and only
    // over each accessor's [acquire, complete] window - so a graph object is free in the
    // gaps (no whole-run reservation). Kick off every root (indegree 0).
    for (size_t i = 0; i < nodes_.size(); ++i)
    {
        if (nodes_[i].indegree == 0)
            on_data_ready(run, static_cast<int>(i));
    }

    return result;
}

} // namespace ts
