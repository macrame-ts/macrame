#pragma once

#include "access.h"
#include "scheduler.h"
#include "thread_safe.h"

#include <cstdint>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ts
{

namespace detail
{

// Extracts the parameter type list of a callable's `operator()`. Works for
// non-generic lambdas, functors, and function pointers; generic lambdas (`auto&`)
// are not introspectable and are unsupported here (see docs/TODO.md).
template<typename T>
struct Function_traits : Function_traits<decltype(&T::operator())> {};

template<typename C, typename R, typename... A>
struct Function_traits<R(C::*)(A...)> { using args = std::tuple<A...>; };

template<typename C, typename R, typename... A>
struct Function_traits<R(C::*)(A...) const> { using args = std::tuple<A...>; };

// free functions / function pointers
template<typename R, typename... A>
struct Function_traits<R(*)(A...)> { using args = std::tuple<A...>; };

} // namespace detail

class Static_task_graph;

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

// Build once, execute many. Nodes declare access to `Thread_safe<>` systems and,
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

    // Add a node: functor + the `Thread_safe<>` instances it accesses. Per-object
    // access mode is deduced from the functor's parameter const-ness
    //   add_node([](Physics& p, const Nav& n){ ... }, physics, nav);   // p:write, n:read
    // Returns a `Graph_node` ordering handle (`after`/`before`).
    template<typename Fn, typename... Ts>
    Graph_node add_node(Fn&& fn, Thread_safe<Ts>&... access)
    {
        using Args = typename detail::Function_traits<std::decay_t<Fn>>::args;
        static_assert(std::tuple_size_v<Args> == sizeof...(Ts),
            "node functor arity must match the number of Thread_safe arguments");

        Node node;
        fill_node<Args>(node, std::index_sequence_for<Ts...>{}, std::forward<Fn>(fn), access...);

        int index = static_cast<int>(nodes_.size());
        nodes_.push_back(std::move(node));
        compiled_ = false;

        return Graph_node(this, index);
    }

    // Resolve access conflicts + explicit edges into a DAG; detect cycles.
    void compile();

    // Run the compiled graph; returns a completion handle. Re-runnable. If `token` is
    // cancelled, not-yet-started nodes are skipped and the completion is cancelled
    // (query with `Task::is_cancelled()`); in-flight nodes still finish.
    Task<void> execute(Scheduler& scheduler = default_scheduler(), Cancellation_token token = {});

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
        int indegree = 0;
        Priority priority = Priority::normal;   // applied to `block` at compile()
        bool inline_dispatch = false;           // run on the settling thread if its acquires all succeed synchronously
        // The node's reusable task block (a `Graph_node_block`, allocated once in
        // compile() and re-armed each run). Its `execute`/`on_complete` are wired so the
        // body may spawn nested tasks and the graph post-logic fires at completion (§7.1).
        std::shared_ptr<detail::Task_control_block> block;
    };

    struct Run_state;

    template<typename Arg>
    static constexpr Access mode_of()
    {
        return std::is_const_v<std::remove_reference_t<Arg>> ? Access::read_only : Access::read_write;
    }

    template<typename Args, std::size_t... I, typename Fn, typename... Ts>
    void fill_node(Node& node, std::index_sequence<I...>, Fn&& fn, Thread_safe<Ts>&... access)
    {
        auto instances = std::make_tuple(&access.instance_...);

        node.access = {
            std::pair<const void*, Access>{
                static_cast<const void*>(std::get<I>(instances)),
                mode_of<std::tuple_element_t<I, Args>>()
            }...
        };

        node.pipes = { (&access.pipe_)... };

        node.run = [fn = std::forward<Fn>(fn), instances]() mutable
        {
            Access_context ctx;
            (ctx.add(static_cast<const void*>(std::get<I>(instances)),
                     mode_of<std::tuple_element_t<I, Args>>()), ...);
            Access_scope scope(ctx);
            fn(*std::get<I>(instances)...);
        };
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
    // (no deferral) -- an inline node dispatches inline only if it stays true to the end.
    static void acquire_next(Run_state& run, int index, int pos, bool synchronous);
    static void run_node(Run_state& run, int index);
    // Graph post-logic for a node whose body AND all its nested tasks have settled:
    // release the objects it held, release its successors, and settle the run when the
    // last node finishes. Runs via the node block's `on_complete` (see run_graph_node).
    static void node_complete(Run_state& run, int index);
    // A node runs as a real task block. These are wired into the block's `execute`
    // (run_graph_node: sets current_task + the execution-flag lock, runs the body, then
    // completes once nested tasks settle) and `on_complete` (graph_node_completed ->
    // node_complete). node_trampoline is the raw scheduler entry -- a fn-ptr + Node*,
    // so dispatching a node costs no per-run allocation.
    static void run_graph_node(const std::shared_ptr<detail::Task_control_block>& block, std::uint64_t generation);
    static void graph_node_completed(detail::Task_control_block* block);
    static void node_trampoline(void* node);

    std::vector<Node> nodes_;
    std::vector<std::pair<int, int>> explicit_edges_;
    std::vector<detail::Pipe*> distinct_pipes_;        // every object the graph touches (indexes pipe acquire)
    std::unique_ptr<Run_state> run_;                   // reused across execute() runs (one run at a time)
    bool compiled_ = false;
};

} // namespace ts
