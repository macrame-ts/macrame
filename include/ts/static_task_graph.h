#pragma once

// The static task graph - the library's build-once / run-many scheduling layer. Nodes declare
// which `Guarded` systems they read and write, and `compile()` derives the execution DAG from
// those declarations (conflicting nodes are ordered, independent ones run in parallel) plus any
// explicit `after`/`before` edges. It exists so a frame's parallel structure follows from
// declared data access instead of being hand-wired: the schedule stays correct as systems
// change, and the same declarations are the safety harness's ground truth. This header carries
// `Static_task_graph`, the `Graph_node` build-time handle, and `Execution_options`.
// User guide: docs/guide.md §6; per-node acquisition and its hazards: docs/internals/task-internals.md §10.

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

class Static_task_graph;

namespace tools
{
class Graph_trace; // Aggregated runtime trace.
}

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

    // Variadic forms: `this` runs after every listed prerequisite (before every listed
    // successor) - independent edges, not a chain among the arguments. So
    // `submit.after(a, b, c)` declares that submit depends on a, b and c together,
    // equivalently to `.after(a).after(b).after(c)` but without reading like a sequence.
    template<typename... Nodes>
        requires (sizeof...(Nodes) > 0) && (std::is_same_v<Nodes, Graph_node> && ...)
    Graph_node& after(const Graph_node& prerequisite, const Nodes&... more);
    template<typename... Nodes>
        requires (sizeof...(Nodes) > 0) && (std::is_same_v<Nodes, Graph_node> && ...)
    Graph_node& before(const Graph_node& successor, const Nodes&... more);

    // Queue priority for this node when it is dispatched each run.
    Graph_node& set_priority(Priority p);

    // Dispatch this node inline: when it becomes ready, run it on the thread that settled
    // its last prerequisite (and acquired its objects) instead of queueing - but only if
    // that thread can acquire all its grants synchronously; if any is contended, it defers 
    // to the queue.
    Graph_node& set_inline();

private:
    friend class Static_task_graph;

    Graph_node(Static_task_graph* graph, int index) noexcept
        : graph_(graph)
        , index_(index)
    {}

    Static_task_graph* graph_ = nullptr;
    int index_ = -1;
};

template<typename... Nodes>
    requires (sizeof...(Nodes) > 0) && (std::is_same_v<Nodes, Graph_node> && ...)
Graph_node& Graph_node::after(const Graph_node& prerequisite, const Nodes&... more)
{
    after(prerequisite);
    (after(more), ...);
    return *this;
}

template<typename... Nodes>
    requires (sizeof...(Nodes) > 0) && (std::is_same_v<Nodes, Graph_node> && ...)
Graph_node& Graph_node::before(const Graph_node& successor, const Nodes&... more)
{
    before(successor);
    (before(more), ...);
    return *this;
}

// Per-run options for `Static_task_graph::execute` (spelled `execute({.token = t})` at call sites).
struct Execution_options
{
    Cancellation_token token;
    // Only meaningful for a nested run - `execute()` called from inside a task (see
    // `execute`). By default such a run is tied to its caller: the caller cannot finish
    // until the run does, and objects the caller already holds are lent to it. `detach =
    // true` cuts both ties for a fire-and-forget run: it may outlive its caller, receives
    // no lend, and queues behind the caller's holds like any unrelated work (on shared
    // objects it typically starts after the caller releases them). Do not await a detached
    // run from its caller: if the graph touches an object the caller holds, that await
    // deadlocks - the caller waits on a run that waits for the caller's own release.
    // Awaiting is what the default (attached) mode is for. Ignored outside a task.
    bool detach = false;
};

// Build once, execute many. Nodes declare access to `Guarded<>` systems and,
// optionally, explicit ordering edges. `compile()` turns access conflicts (plus
// explicit edges) into a DAG; `execute()` runs it, parallelizing independent
// nodes. A node returns no result - its output is the writes to the systems it
// declares (typed node results are future work, docs/TODO.md 2.1).
class Static_task_graph
{
    friend class Graph_node;

public:
    // Movable so a graph can be built-and-returned (e.g. `build_frame_graph`).
    // Under `TS_SAFETY_CHECKS` the destructor, the move constructor and a move-assign
    // overwrite all fatal while a run is in flight (scheduled node blocks keep the
    // source's address until the next `execute()` refreshes them).
    Static_task_graph();
    ~Static_task_graph();
    Static_task_graph(Static_task_graph&&) noexcept;
    Static_task_graph& operator=(Static_task_graph&&) noexcept;

    // Add a node: a leading `ts::Named`, the functor, and the `Guarded<>` instances it
    // accesses. The name is required - a literal, or `{}` to identify the node by this
    // call site - because it labels the node in the DOT dump, the trace, and every
    // diagnostic that names a task (the node's block carries it).
    //
    // Per-object access mode, one rule (same as `Guarded::access`): parameter const-ness
    // for a non-generic functor
    //   add_node("pose", [](Physics& p, const Nav& n){ ... }, physics, nav);  // p:write, n:read
    // (by-value / `T&&` resource parameters are rejected); the rvalue probe for a generic one
    //   add_node({}, [](const auto& p, auto& a){ a.pose(p); }, physics, anim);  // p:read, a:write
    // or explicit `ts::as_read_only`/`as_read_write` tags on every object (don't mix tagged and
    // bare in one node). Read positions receive `const T&`, so a mutating body under a read
    // classification does not compile. A body may declare an optional trailing
    // `Cancellation_token` after the resource parameters to receive the run's token (see
    // `execute`). Returns a `Graph_node` ordering handle.
    template<typename Fn, typename... Objs>
        requires (detail::Object_arg<Objs> && ...)
    Graph_node add_node(Named name, Fn&& fn, Objs&&... objs);

    // Resolve access conflicts + explicit edges into a DAG; detect cycles. Called exactly
    // once, after every `add_node`/`after`/`before`: the graph is build-once, so further
    // building - or a second `compile()` - is fatal; build a new graph instance instead.
    // A non-null `DOT_path` also writes the compiled structure as a Graphviz DOT file (see
    // `tools/dot_writer.h` for the style scheme); no-op when `TS_PROFILING` is 0.
    void compile(const char* DOT_path = nullptr);

    // Run the compiled graph; returns a completion handle. Re-runnable.
    //
    // Cancellation is cooperative, through the run's token: each node checks it as it is
    // about to run, so not-yet-started nodes are skipped (they settle cancelled) while nodes
    // already in flight finish normally; the completion handle then settles cancelled (query
    // with `Task::is_cancelled()`). A body that wants a mid-body early-out opts in by
    // declaring a trailing `Cancellation_token` parameter after its resource parameters -
    // the same spelling as `Guarded::async` accessors and `ts::launch` bodies - and receives
    // this run's token to poll; a cooperative mid-body return settles the node completed
    // (it ran), not cancelled.
    //
    // Runs on the process-wide scheduler. To run under a different configuration for a scope,
    // reconfigure it with `Scheduler_scope` - another worker count, or the worker-less
    // `{.single_threaded = true}` mode (the scheduler owns no threads and every task executes
    // inline at submit, so the whole run happens deterministically on the calling thread).
    //
    // nested runs (docs/internals/coroutine-first.md §4.8). Calling `execute()` from inside a task -
    // typically `co_await inner.execute()` in a graph node - is supported, and two things
    // happen by default:
    //  - **Lending.** Every object this graph declares that the calling task already holds a
    //    covering grant on (write covers read and write, read covers read) is lent for the
    //    run: the inner nodes skip taking their own turns on it, because the caller's grant
    //    already excludes everyone else. Without this an inner node would queue behind the
    //    grant its own caller is holding - a deadlock. The compiled conflict edges still
    //    order the inner nodes among themselves on a lent object, and the inner nodes'
    //    access contexts carry the caller's grant window, so the harness stays live.
    //    Recursion works by construction. Concurrent `async` accesses are unaffected: they
    //    queue behind the caller's hold exactly as before. Await a lent run before the
    //    caller's own body touches the lent objects again - the lend hands the caller's
    //    exclusivity to the run, so a caller body racing its own lent run goes undetected;
    //    the sanctioned spelling is `co_await inner.execute()` directly.
    //  - **Scope join.** The run joins the calling task's implicit scope, so the caller
    //    cannot complete (and a node cannot release its objects) while the inner run is
    //    still going. An un-awaited inner run therefore cannot float; pass `{.detach = true}`
    //    when you deliberately want one to outlive its launcher, which also forgoes lending.
    // Three misuses are fatal under `TS_SAFETY_CHECKS`: a mode-incompatible overlap (the
    // caller holds read where the inner graph writes - restructure so the caller declares
    // the write, or hoist the writer out of the sub-graph); lending while an earlier un-awaited
    // nested run of the caller is still in flight (`co_await` the previous run first - it could
    // race the lent-to graph on the same object, both "validly"); and calling `execute()` on a graph
    // whose previous run is still in flight (one run at a time - use a second instance, or
    // order the callers).
    //
    // `[[nodiscard]]`: the returned handle is the run's only completion signal, and the
    // one-run rule means the next `execute()` is legal only after this run is awaited or
    // sync()ed - dropping the handle forfeits both. A deliberate fire-and-forget (a detached
    // run whose completion genuinely does not matter) spells its intent with `(void)`.
    [[nodiscard]] Task<void> execute(Execution_options opts = {});

    // Attach an aggregating runtime trace (tools/graph_trace.h), or detach with nullptr.
    // Requires a compiled graph: the compiled structure (node labels, declared accesses,
    // edge provenance) is pushed into the trace immediately, then each completed run is
    // folded into it at settle (cancelled runs are skipped). The trace must outlive its
    // attachment; it is not owned. With `TS_PROFILING` 0 the attachment is accepted but
    // records nothing (stamps and fold compile out).
    void set_trace(tools::Graph_trace* trace);

    int node_count() const { return static_cast<int>(nodes_.size()); }

private:
    struct Node
    {
        // --- captured from `add_node` (the user's declaration) ---------------------------
        Named name{ nullptr };                  // literal or `add_node` call site; empty if unnamed
        // The body, wrapped with its access scope. Receives the run's token (off the node
        // block, re-armed each `execute()`); the wrapper forwards it only to a body that
        // declared the opt-in trailing `Cancellation_token` parameter.
        std::move_only_function<void(const Cancellation_token&)> run;
        // Per declared object, in argument order: (instance, mode). The instance is the
        // type-erased address of the `Guarded`'s stored `T` - the same identity the harness
        // (`Access_context`) keys by; `const void*` because the graph is type-erased over `T`.
        // Drives conflict-edge derivation and the body's access context.
        std::vector<std::pair<const void*, Access>> access;
        // The declared objects' pipes, parallel to `access` (argument order; distinct by
        // construction - a duplicate object is fatal at `add_node`). Raw input for
        // `compile()`, which derives the canonical form below and releases this (the graph
        // is build-once, so it is never needed again).
        std::vector<detail::Pipe*> pipes;
        Priority priority = Priority::normal;   // applied to `block` at each run's re-arm
        bool inline_dispatch = false;           // run on the settling thread if its acquires all succeed synchronously

        // --- derived by `compile()` / used by the run machinery --------------------------
        std::vector<int> pipe_indices;      // `pipes` deduped as indices into distinct_pipes_ (ascending = canonical acquire order)
        std::vector<Access> pipe_modes;     // this node's access mode per pipe_indices entry
        std::vector<int> successors;        // edges out of this node (targets' indices), conflict-derived + explicit
        int indegree = 0;                   // edges in; each run seeds `remaining_deps` from it (0 = a root)
        std::vector<int> ready_buf;         // scratch: successors made ready by this node's completion (reused; single completion/run)
        // The node's reusable task block (a `Graph_node_block`, allocated once in
        // compile() and re-armed each run). Its `execute`/`on_complete` are wired so the
        // body may spawn nested tasks and the graph post-logic fires at completion (§7.1).
        detail::Task_ptr block;
    };

    struct Run_state;

    // The one node builder: access list, pipes, and the body from the per-object
    // (compile-time) `Modes...`. Read positions are invoked with `const T&` (`mode_ref`), so a
    // mutating body under a read classification fails to compile - structural, on top of the
    // harness. The deduced / probed / tagged wrappers below differ only in the `Modes` source,
    // so `compile()` derives identical edges and exclusion for all three. `Takes_token` marks
    // a body that declared the opt-in trailing `Cancellation_token` (each tier detects it);
    // the wrapper then forwards the run's token as the extra trailing argument.
    template<bool Takes_token, Access... Modes, std::size_t... I, typename Fn, typename... Ts>
    void fill_node_modes(Node& node, std::index_sequence<I...>, Fn&& fn, Guarded<Ts>&... access);

    // Deduced (bare args, introspectable functor): modes from parameter const-ness; by-value /
    // rvalue-ref resource parameters rejected. `I` spans the resource parameters only, so a
    // trailing token parameter (detected by `add_node`) never reaches mode deduction.
    template<typename Args, bool Takes_token, std::size_t... I, typename Fn, typename... Ts>
    void fill_node(Node& node, std::index_sequence<I...> seq, Fn&& fn, Guarded<Ts>&... access);

    // Probed (bare args, generic functor): modes from the per-position rvalue probe -
    // `const auto&`/`auto&&` = read, `auto&` = write. No tags needed.
    template<std::size_t... I, typename Fn, typename... Ts>
    void fill_node_probed(Node& node, std::index_sequence<I...> seq, Fn&& fn, Guarded<Ts>&... access);

    // Tagged (`ts::as_read_only`/`as_read_write` on every arg): modes from the tags.
    template<std::size_t... I, typename Fn, typename... Objs>
    void fill_node_tagged(Node& node, std::index_sequence<I...> seq, Fn&& fn, Objs&&... objs);

    void add_edge(int prerequisite, int successor);
    static bool conflicts(const Node& a, const Node& b);
    void detect_cycles() const;

    // Per-node acquire: when a node becomes data-ready it acquires the objects it touches,
    // one at a time in canonical (ascending pipe-index) order, holding each - mode-aware
    // (a reader joins concurrent readers, a writer is exclusive). Once all are held the node
    // runs; on completion it releases them. So an object is held only over each accessor's
    // [acquire, complete] window - free in the gaps for async / other objects (no whole-run
    // reservation). See docs §10.
    static void on_data_ready(Run_state& run, int index);
    // Graph post-logic for a node whose body and all its nested tasks have settled:
    // release the objects it held, release its successors, and settle the run when the
    // last node finishes. Runs via the node block's `on_complete` (see run_graph_node).
    static void node_complete(Run_state& run, int index);
    // A node runs as a real task block. These are wired into the block's `execute`
    // (run_graph_node: sets `Current_task` + the execution-flag lock, runs the body, then
    // completes once nested tasks settle) and `on_complete` (graph_node_completed ->
    // node_complete). node_trampoline is the raw scheduler entry - a fn-ptr + Node*,
    // so dispatching a node costs no per-run allocation.
    static void run_graph_node(const detail::Task_ptr& block);
    static void graph_node_completed(detail::Task_control_block* block);

    // Resolve this run's lend set from the calling task's grants and (re)bind the node link
    // slab to the objects the run must actually take turns on. Called at the top of every
    // `execute()`; a plain top-level run resolves an empty lend set and rebinds nothing.
    void bind_links_for_run(bool detach);

    // Fatal (`TS_SAFETY_CHECKS`) if a run is in flight - `where` names the misuse; a no-op
    // in shipping. The move constructor calls it before the members leave the source.
    void check_quiescent(const char* where) noexcept;
#if TS_SAFETY_CHECKS
    // `check_quiescent`, then balance the pipes' `graph_refs` for the current
    // `distinct_pipes_`. Called by the destructor (which a move-assign overwrite routes
    // through); not by the move constructor - the registrations ride the move.
    void check_quiescent_and_release_pipes(const char* where) noexcept;
#endif

    std::vector<Node> nodes_;
    std::vector<std::pair<int, int>> explicit_edges_;
    std::vector<detail::Pipe*> distinct_pipes_;   // every pipe the graph touches (address-sorted; index = canonical id)
    // Parallel to `distinct_pipes_`: per pipe, the type-erased address of the object's stored
    // `T` (the identity `Access_context` keys by - the graph is type-erased over `T`, hence
    // `const void*`) and the strongest mode any node uses on it. Consumed by the nested-run
    // lend decision (`bind_links_for_run`): a lend requires the caller's grant to cover the
    // strongest access this graph performs on the object (a read grant cannot cover a writer).
    std::vector<const void*> pipe_instances_;
    std::vector<Access> pipe_modes_;
    // Every node's pipe links, contiguous per node: bound at compile() (`block->pipe_links`
    // points into this), re-armed each execute() - runs stay allocation-free.
    std::unique_ptr<detail::Pipe_link[]> node_links_;
    // Nested-run lend state (see `execute`). One flag per `distinct_pipes_` entry, recomputed
    // per run (`std::uint8_t` because these are boolean flags and `vector<bool>` is a packed
    // proxy, not addressable bytes); `links_lent_` records whether the link slab is currently
    // bound to a lent subset, so the next plain run knows it must rebind the full set back.
    std::vector<std::uint8_t> lent_;
    bool links_lent_ = false;
    std::unique_ptr<Run_state> run_;                   // reused across execute() runs (one run at a time)
    bool compiled_ = false;
    // Attached via set_trace; not owned. Unconditional (one pointer) so the run logic
    // needs no `TS_PROFILING` blocks; without profiling it is stored but never read.
    tools::Graph_trace* trace_ = nullptr;
};

// --- member template definitions ------------------------------------------------------------

template<typename Fn, typename... Objs>
    requires (detail::Object_arg<Objs> && ...)
Graph_node Static_task_graph::add_node(Named name, Fn&& fn, Objs&&... objs)
{
    if (compiled_)
        ts::fatal("Static_task_graph::add_node on a compiled graph - the graph is build-once "
                  "(add every node before compile(), or build a new graph)");

    Node node;
    constexpr bool any_tagged = (detail::is_access_arg_v<Objs> || ...);
    if constexpr (any_tagged)
    {
        static_assert((detail::is_access_arg_v<Objs> && ...),
            "add_node: don't mix tagged (ts::as_read_only/as_read_write) and bare Guarded arguments "
            "- tag EVERY object argument, or tag none");
        fill_node_tagged(node, std::index_sequence_for<Objs...>{},
            std::forward<Fn>(fn), std::forward<Objs>(objs)...);
    }
    else if constexpr (detail::introspectable_v<Fn>)
    {
        using Args = typename detail::Function_traits<std::decay_t<Fn>>::args;
        constexpr bool takes_token = detail::last_arg_is_token_v<Args, sizeof...(Objs)>;
        static_assert(std::tuple_size_v<Args> == sizeof...(Objs) + (takes_token ? 1 : 0),
            "node functor arity must match the number of Guarded arguments "
            "(plus an optional trailing Cancellation_token)");
        fill_node<Args, takes_token>(node, std::index_sequence_for<Objs...>{},
            std::forward<Fn>(fn), objs...);
    }
    else
    {
        fill_node_probed(node, std::index_sequence_for<Objs...>{},
            std::forward<Fn>(fn), objs...);
    }

    // A repeated object is fatal (same strictness as the multi-object verbs): a duplicate
    // is a copy-paste bug far more often than intent, and it would double the object in the
    // declaration record (DOT, trace, diagnostics). Cold path, <= 8 declarations.
    for (std::size_t i = 0; i < node.access.size(); ++i)
    {
        for (std::size_t j = i + 1; j < node.access.size(); ++j)
        {
            if (node.access[i].first == node.access[j].first)
                ts::fatal("add_node: the same Guarded object is declared twice on one node - "
                          "declare each object once, with the strongest access the body needs");
        }
    }

    node.name = name;
    int index = static_cast<int>(nodes_.size());
    nodes_.push_back(std::move(node));

    return Graph_node(this, index);
}

template<bool Takes_token, Access... Modes, std::size_t... I, typename Fn, typename... Ts>
void Static_task_graph::fill_node_modes(Node& node, std::index_sequence<I...>, Fn&& fn, Guarded<Ts>&... access)
{
    auto instances = std::make_tuple(&access.instance_...);

    // A task-returning body must be exactly `Task<void>` - that is the coroutine-node
    // shape, gated on below via `add_nested` so the node completes when the frame does. A
    // `Task<R>` body would take the plain branch: the returned task discarded, the node
    // completing (and releasing its grants) while the eager frame still runs - a stale
    // inherited grant. Nodes carry no results by design; typed chaining is docs/TODO.md 2.1.
    using Body_result = typename detail::Node_body_result<Takes_token, Fn&, detail::Mode_ref_t<Modes, Ts>...>::type;
    static_assert(!detail::is_task_v<Body_result> || std::is_same_v<Body_result, Task<void>>,
        "a coroutine node body must return ts::Task<void>; a Task<R> result would be "
        "discarded and the frame would outrun the node's grants (typed node results are "
        "future work - docs/TODO.md 2.1)");

    node.access = {
        std::pair<const void*, Access>{
            static_cast<const void*>(std::get<I>(instances)), Modes
        }...
    };

    node.pipes = { (&access.pipe_)... };

    auto epochs = std::make_tuple(detail::pipe_epoch(access.pipe_)...);
    auto ranks = std::make_tuple(detail::pipe_rank(access.pipe_)...);
    node.run = [fn = std::forward<Fn>(fn), instances, epochs, ranks](
        [[maybe_unused]] const Cancellation_token& token) mutable
    {
        Access_context ctx;
        (ctx.add(static_cast<const void*>(std::get<I>(instances)), Modes,
                 std::get<I>(epochs), std::get<I>(ranks)), ...);
        Access_scope scope(ctx);
        // One invocation spelling for both arities: a token-taking body (opt-in trailing
        // `Cancellation_token`) receives the run's token - by value into a coroutine
        // body's frame - and may return cooperatively mid-body (the node then settles
        // completed, not cancelled, exactly like `Executable::run`'s model).
        auto call_body = [&]() -> decltype(auto)
        {
            if constexpr (Takes_token)
                return fn(detail::mode_ref<Modes>(std::get<I>(instances))..., token);
            else
                return fn(detail::mode_ref<Modes>(std::get<I>(instances))...);
        };
        if constexpr (std::is_same_v<decltype(call_body()), Task<void>>)
        {
            // A coroutine node body (docs/internals/coroutine-first.md §4.4): the returned frame's
            // task gates the node's completion via the nested mechanism - the node
            // completes (releasing grants and successors) when the frame completes, not
            // at the first suspension. The frame inherits the node's grant snapshot at
            // creation, so resumed segments keep the declared accesses.
            detail::add_nested(detail::core_of(detail::invoke_user_body(call_body)));
        }
        else
        {
            detail::invoke_user_body(call_body);
        }
    };
}

template<typename Args, bool Takes_token, std::size_t... I, typename Fn, typename... Ts>
void Static_task_graph::fill_node(Node& node, std::index_sequence<I...> seq, Fn&& fn, Guarded<Ts>&... access)
{
    static_assert((std::is_lvalue_reference_v<std::tuple_element_t<I, Args>> && ...),
        "a guarded-resource parameter must be `T&` or `const T&`: taking it by value copies "
        "the resource (writes hit the copy and are silently discarded), and `T&&` cannot "
        "bind the stored instance");
    fill_node_modes<Takes_token, detail::async_mode_of<std::tuple_element_t<I, Args>>()...>(
        node, seq, std::forward<Fn>(fn), access...);
}

template<std::size_t... I, typename Fn, typename... Ts>
void Static_task_graph::fill_node_probed(Node& node, std::index_sequence<I...> seq, Fn&& fn, Guarded<Ts>&... access)
{
    // Token-taking wins for a variadic-generic functor that accepts both arities - the
    // `accessor_takes_token_v` precedent.
    constexpr bool takes_token = std::invocable<Fn, Ts&..., const Cancellation_token&>;
    static_assert(takes_token || std::invocable<Fn, Ts&...>,
        "node functor parameters must match the Guarded arguments "
        "(same arity, each taken by reference, plus an optional trailing Cancellation_token)");
    // Guard the forwards on the same conditions: a failed assert does not stop
    // instantiation, so without them `fill_node_modes` re-errors past the message.
    if constexpr (takes_token)
    {
        // The rvalue probe must carry the trailing token, or a token-taking generic lambda
        // fails the probe's arity and every position mis-probes as a write. `Cancellation_token`
        // joins as one more probe argument: `I` still ranges over the resource positions only,
        // so the token position always receives an lvalue, which binds the body's by-value
        // (or `const&`) token parameter in every probe invocation.
        fill_node_modes<true, detail::probed_mode<Fn, I, Ts..., Cancellation_token>()...>(
            node, seq, std::forward<Fn>(fn), access...);
    }
    else if constexpr (std::invocable<Fn, Ts&...>)
    {
        fill_node_modes<false, detail::probed_mode<Fn, I, Ts...>()...>(
            node, seq, std::forward<Fn>(fn), access...);
    }
}

template<std::size_t... I, typename Fn, typename... Objs>
void Static_task_graph::fill_node_tagged(Node& node, std::index_sequence<I...> seq, Fn&& fn, Objs&&... objs)
{
    // Probe token-taking with the tagged mode refs - the exact refs the body will be
    // invoked with, so the probe instantiates nothing the real invocation would not.
    constexpr bool takes_token = std::invocable<Fn,
        detail::Mode_ref_t<std::remove_cvref_t<Objs>::mode, typename std::remove_cvref_t<Objs>::value_type>...,
        const Cancellation_token&>;
    fill_node_modes<takes_token, std::remove_cvref_t<Objs>::mode...>(
        node, seq, std::forward<Fn>(fn), *objs.obj...);
}

} // namespace ts
