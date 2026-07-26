#pragma once

#include "ts/access.h"
#include "ts/scheduler.h"
#include "ts/guarded.h"

#include <cstdint>
#include <memory>
#include <source_location>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// Profiling / introspection instrumentation (the DOT structure dump today; runtime capture
// is future work). On by default; define TS_PROFILING=0 in shipping builds to compile it
// out (the `compile(DOT_path)` parameter stays in the signature and becomes a no-op).
#ifndef TS_PROFILING
#define TS_PROFILING 1
#endif

namespace ts
{

// `detail::Function_traits` (per-argument type extraction for access-mode deduction) now
// lives in `guarded.h` -- shared with multi-object `ts::async`.

class Static_task_graph;

namespace tools
{
// Aggregated runtime trace (tools/graph_trace.h). Forward-declared so the public header
// does not depend on the tools folder; only static_task_graph.cpp includes it.
class Graph_trace;
}

// A node's debug name: an optional static-storage string plus the `add_node` call site.
// Implicit from a literal -- `g.add_node("propagation", fn, objs...)` -- and
// default-constructible: `g.add_node({}, fn, objs...)` captures the call site alone, so
// the node still labels usefully (`file:line`). The string is referenced, not copied --
// pass a literal (or anything outliving the graph).
struct Node_name
{
    Node_name(std::source_location site = std::source_location::current()) noexcept
        : site(site)
    {}
    Node_name(const char* name, std::source_location site = std::source_location::current()) noexcept
        : literal(name)
        , site(site)
    {}

    const char* literal = nullptr;
    std::source_location site;
};

// Handle to a node in a `Static_task_graph`, returned by `add_node`. Identifies
// the node for explicit ordering edges (`after`/`before`). It is build-time
// identity, not a completion handle: the graph is build-once / run-many, so a
// node has no single result (consume a graph run via the `Task<void>` from
// `execute()`). Default-constructed handles are inert.
class Graph_node
{
public:
    Graph_node() = default;

    // `this` runs after `prerequisite` / before `successor` (same graph).
    Graph_node& after(const Graph_node& prerequisite);
    Graph_node& before(const Graph_node& successor);

    // Queue priority for this node when it is dispatched each run.
    Graph_node& priority(Priority p);

    // Dispatch this node INLINE: when it becomes ready, run it on the thread that settled
    // its last prerequisite (and acquired its objects) instead of queueing -- but only if
    // that thread can acquire ALL its objects synchronously; if any is contended (held by
    // async), it defers to the queue. Bounded by the shared inline trampoline. Same
    // trade-offs as `Task_builder::set_inline` (nondeterministic thread, must not block).
    Graph_node& set_inline();

    int index() const noexcept { return index_; }

private:
    friend class Static_task_graph;

    Graph_node(Static_task_graph* graph, int index) noexcept
        : graph_(graph)
        , index_(index)
    {}

    Static_task_graph* graph_ = nullptr;
    int index_ = -1;
};

// Build once, execute many. Nodes declare access to `Guarded<>` systems and,
// optionally, explicit ordering edges. `compile()` turns access conflicts (plus
// explicit edges) into a DAG; `execute()` runs it, parallelizing independent
// nodes. Nodes are void (they mutate the systems they access).
class Static_task_graph
{
    friend class Graph_node;

public:
    // Declared out-of-line (defaulted in the .cpp) because the reused `run_`
    // (unique_ptr<Run_state>) has an incomplete pointee here. Movable so a graph can be
    // built-and-returned (e.g. build_frame_graph); execute() refreshes the blocks' back
    // pointers, so a moved graph is valid on its next run.
    Static_task_graph();
    ~Static_task_graph();
    Static_task_graph(Static_task_graph&&) noexcept;
    Static_task_graph& operator=(Static_task_graph&&) noexcept;

    // Add a node: functor + the `Guarded<>` instances it accesses. Per-object access mode, one
    // rule (same as `Guarded::access`): parameter const-ness for a non-generic functor
    //   add_node([](Physics& p, const Nav& n){ ... }, physics, nav);   // p:write, n:read
    // (by-value / `T&&` resource parameters are rejected); the rvalue probe for a GENERIC one
    //   add_node([](const auto& p, auto& a){ a.pose(p); }, physics, anim);   // p:read, a:write
    // or explicit `ts::as_read_only`/`as_read_write` tags on every object (don't mix tagged and
    // bare in one node). Read positions receive `const T&`, so a mutating body under a read
    // classification does not compile. Returns a `Graph_node` ordering handle.
    template<typename Fn, typename... Objs>
        requires (detail::Object_arg<Objs> && ...)
    Graph_node add_node(Fn&& fn, Objs&&... objs)
    {
        Node node;
        constexpr bool any_tagged = (detail::is_access_arg_v<Objs> || ...);
        if constexpr (any_tagged)
        {
            static_assert((detail::is_access_arg_v<Objs> && ...),
                "add_node: don't mix tagged (ts::as_read_only/as_read_write) and bare Guarded arguments "
                "-- tag EVERY object argument, or tag none");
            fill_node_tagged(node, std::index_sequence_for<Objs...>{},
                std::forward<Fn>(fn), std::forward<Objs>(objs)...);
        }
        else if constexpr (detail::introspectable_v<Fn>)
        {
            using Args = typename detail::Function_traits<std::decay_t<Fn>>::args;
            static_assert(std::tuple_size_v<Args> == sizeof...(Objs),
                "node functor arity must match the number of Guarded arguments");
            fill_node<Args>(node, std::index_sequence_for<Objs...>{},
                std::forward<Fn>(fn), objs...);
        }
        else
        {
            fill_node_probed(node, std::index_sequence_for<Objs...>{},
                std::forward<Fn>(fn), objs...);
        }

        int index = static_cast<int>(nodes_.size());
        nodes_.push_back(std::move(node));
        compiled_ = false;

        return Graph_node(this, index);
    }

    // Named form: the leading `Node_name` (implicit from a string literal, or `{}` for the
    // call site alone) labels the node in the DOT dump; unnamed nodes label as `node<N>`.
    template<typename Fn, typename... Objs>
        requires (detail::Object_arg<Objs> && ...)
    Graph_node add_node(Node_name name, Fn&& fn, Objs&&... objs)
    {
        Graph_node handle = add_node(std::forward<Fn>(fn), std::forward<Objs>(objs)...);
        Node& node = nodes_[static_cast<std::size_t>(handle.index())];
        node.name = name.literal;
        node.name_site = name.site;
        node.has_name_site = true;
        return handle;
    }

    // Resolve access conflicts + explicit edges into a DAG; detect cycles. A non-null
    // `DOT_path` also writes the compiled structure as a Graphviz DOT file (see
    // `tools/dot_writer.h` for the style scheme); no-op when `TS_PROFILING` is 0.
    void compile(const char* DOT_path = nullptr);

    // Per-run options for `execute`. An aggregate (house style: `Launch_options`,
    // `Access_options`); spelled `execute({.token = t})` at call sites.
    struct Execution_options
    {
        Cancellation_token token;
    };

    // Run the compiled graph; returns a completion handle. Re-runnable. If the token is
    // cancelled, not-yet-started nodes are skipped and the completion is cancelled
    // (query with `Task::is_cancelled()`); in-flight nodes still finish.
    // Runs on the one global scheduler (there are no ad-hoc `Scheduler` instances; use a
    // `Scheduler_scope` to run on a specific pool for a scope -- including a worker-less
    // `{.single_threaded = true}` one, which runs the whole graph deterministically on the
    // calling thread).
    Task<void> execute(Execution_options opts = {});

    // Attach an aggregating runtime trace (tools/graph_trace.h), or detach with nullptr.
    // Requires a compiled graph: the compiled structure (node labels, declared accesses,
    // edge provenance) is pushed into the trace immediately, then each completed run is
    // folded into it at settle (cancelled runs are skipped). A recompile re-pushes the
    // structure, resetting the trace's aggregates. The trace must outlive its attachment;
    // it is not owned. With `TS_PROFILING` 0 the attachment is accepted but records
    // nothing (stamps and fold compile out).
    void set_trace(tools::Graph_trace* trace);

    int node_count() const { return static_cast<int>(nodes_.size()); }

private:
    struct Node
    {
        std::move_only_function<void()> run;
        std::vector<std::pair<const void*, Access>> access;
        std::vector<detail::Pipe*> pipes;   // the pipes of the objects this node accesses
        std::vector<int> pipe_indices;      // those pipes as indices into distinct_pipes_ (deduped, ASCENDING = canonical acquire order)
        std::vector<Access> pipe_modes;     // this node's mode per pipe_indices entry (write wins on a dup)
        std::vector<int> successors;
        std::vector<int> ready_buf;             // scratch: successors made ready by this node's completion (reused; single completion/run)
        int indegree = 0;
        const char* name = nullptr;             // static literal from `Node_name`, or null
        std::source_location name_site{};       // the named add_node's call site
        bool has_name_site = false;             // set only by the named overload
        Priority priority = Priority::normal;   // applied to `block` at compile()
        bool inline_dispatch = false;           // run on the settling thread if its acquires all succeed synchronously
        // The node's reusable task block (a `Graph_node_block`, allocated once in
        // compile() and re-armed each run). Its `execute`/`on_complete` are wired so the
        // body may spawn nested tasks and the graph post-logic fires at completion (§7.1).
        detail::Task_ptr block;
    };

    struct Run_state;

    // The one node builder: access list, pipes, and the body from the per-object
    // (compile-time) `Modes...`. Read positions are invoked with `const T&` (`mode_ref`), so a
    // mutating body under a read classification fails to compile -- structural, on top of the
    // harness. The deduced / probed / tagged wrappers below differ only in the `Modes` source,
    // so `compile()` derives identical edges and exclusion for all three.
    template<Access... Modes, std::size_t... I, typename Fn, typename... Ts>
    void fill_node_modes(Node& node, std::index_sequence<I...>, Fn&& fn, Guarded<Ts>&... access)
    {
        auto instances = std::make_tuple(&access.instance_...);

        node.access = {
            std::pair<const void*, Access>{
                static_cast<const void*>(std::get<I>(instances)), Modes
            }...
        };

        node.pipes = { (&access.pipe_)... };

        auto epochs = std::make_tuple(&access.pipe_.write_epoch...);
        node.run = [fn = std::forward<Fn>(fn), instances, epochs]() mutable
        {
            Access_context ctx;
            (ctx.add(static_cast<const void*>(std::get<I>(instances)), Modes,
                     std::get<I>(epochs)), ...);
            Access_scope scope(ctx);
            fn(detail::mode_ref<Modes>(std::get<I>(instances))...);
        };
    }

    // Deduced (bare args, introspectable functor): modes from parameter const-ness; by-value /
    // rvalue-ref resource parameters rejected.
    template<typename Args, std::size_t... I, typename Fn, typename... Ts>
    void fill_node(Node& node, std::index_sequence<I...> seq, Fn&& fn, Guarded<Ts>&... access)
    {
        static_assert((std::is_lvalue_reference_v<std::tuple_element_t<I, Args>> && ...),
            "a guarded-resource parameter must be `T&` or `const T&`: taking it by value copies "
            "the resource (writes hit the copy and are silently discarded), and `T&&` cannot "
            "bind the stored instance");
        fill_node_modes<detail::async_mode_of<std::tuple_element_t<I, Args>>()...>(
            node, seq, std::forward<Fn>(fn), access...);
    }

    // Probed (bare args, GENERIC functor): modes from the per-position rvalue probe --
    // `const auto&`/`auto&&` = read, `auto&` = write. No tags needed.
    template<std::size_t... I, typename Fn, typename... Ts>
    void fill_node_probed(Node& node, std::index_sequence<I...> seq, Fn&& fn, Guarded<Ts>&... access)
    {
        static_assert(std::invocable<Fn, Ts&...>,
            "node functor parameters must match the Guarded arguments "
            "(same arity, each taken by reference)");
        fill_node_modes<detail::probed_mode<Fn, I, Ts...>()...>(
            node, seq, std::forward<Fn>(fn), access...);
    }

    // Tagged (`ts::as_read_only`/`as_read_write` on every arg): modes from the tags.
    template<std::size_t... I, typename Fn, typename... Objs>
    void fill_node_tagged(Node& node, std::index_sequence<I...> seq, Fn&& fn, Objs&&... objs)
    {
        fill_node_modes<std::remove_cvref_t<Objs>::mode...>(
            node, seq, std::forward<Fn>(fn), *objs.obj...);
    }

    void add_edge(int prerequisite, int successor);
    static bool conflicts(const Node& a, const Node& b);
    void detect_cycles() const;

    // Per-node acquire: when a node becomes data-ready it acquires the objects it touches,
    // one at a time in canonical (ascending pipe-index) order, holding each -- mode-aware
    // (a reader joins concurrent readers, a writer is exclusive). Once all are held the node
    // runs; on completion it releases them. So an object is held only over each accessor's
    // [acquire, complete] window -- free in the gaps for async / other objects (no whole-run
    // reservation). See docs §10.
    static void on_data_ready(Run_state& run, int index);
    // `synchronous` tracks whether the whole acquire chain so far ran on the settling thread
    // (no deferral) -- an inline node dispatches inline only if it stays true to the end. A
    // pre-held object (handed from a predecessor, see `node_complete`) is skipped without a
    // pipe op and keeps `synchronous`.
    static void acquire_next(Run_state& run, int index, int pos, bool synchronous);
    static void run_node(Run_state& run, int index);
    // Object handoff (elide a release + re-acquire round-trip): the ready successor to hand
    // object `pi` (held in mode `m`) to, or -1. Handoff iff exactly one ready successor
    // accesses `pi`, in the same mode `m` -- then the pipe state (writer/reader) is already
    // right for it, so releasing and re-acquiring is pure waste. `mark_preheld` sets the bit
    // that makes that successor's `acquire_next` skip the object.
    static int handoff_target(Run_state& run, const std::vector<int>& ready, int pi, Access m);
    static void mark_preheld(Run_state& run, int node_index, int pi);
    // Graph post-logic for a node whose body AND all its nested tasks have settled:
    // release the objects it held, release its successors, and settle the run when the
    // last node finishes. Runs via the node block's `on_complete` (see run_graph_node).
    static void node_complete(Run_state& run, int index);
    // A node runs as a real task block. These are wired into the block's `execute`
    // (run_graph_node: sets current_task + the execution-flag lock, runs the body, then
    // completes once nested tasks settle) and `on_complete` (graph_node_completed ->
    // node_complete). node_trampoline is the raw scheduler entry -- a fn-ptr + Node*,
    // so dispatching a node costs no per-run allocation.
    static void run_graph_node(const detail::Task_ptr& block, std::uint64_t generation);
    static void graph_node_completed(detail::Task_control_block* block);
    static void node_trampoline(void* node);

    std::vector<Node> nodes_;
    std::vector<std::pair<int, int>> explicit_edges_;
    std::vector<detail::Pipe*> distinct_pipes_;        // every object the graph touches (indexes pipe acquire)
    std::unique_ptr<Run_state> run_;                   // reused across execute() runs (one run at a time)
    bool compiled_ = false;
    // Attached via set_trace; not owned. Unconditional (one pointer) so the run logic
    // needs no `TS_PROFILING` blocks; without profiling it is stored but never read.
    tools::Graph_trace* trace_ = nullptr;
};

} // namespace ts
