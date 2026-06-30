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

// Extracts the parameter type list of a callable's operator(). Works for
// non-generic lambdas, functors, and function pointers; generic lambdas (auto&)
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

// Build once, execute many. Nodes declare access to Thread_safe<> systems and,
// optionally, explicit ordering edges. compile() turns access conflicts (plus
// explicit edges) into a DAG; execute() runs it, parallelizing independent nodes.
// Nodes are void (they mutate the systems they access).
class Static_task_graph
{
public:
    // Add a node: functor + the Thread_safe<> instances it accesses. Per-object
    // access mode is deduced from the functor's parameter const-ness
    //   add_node([](Physics& p, const Nav& n){ ... }, physics, nav);   // p:write, n:read
    // Returns a Task<void> usable only as an ordering handle (after/before).
    template<typename Fn, typename... Ts>
    Task<void> add_node(Fn&& fn, Thread_safe<Ts>&... access)
    {
        using Args = typename detail::Function_traits<std::decay_t<Fn>>::args;
        static_assert(std::tuple_size_v<Args> == sizeof...(Ts),
            "node functor arity must match the number of Thread_safe arguments");

        Node node;
        fill_node<Args>(node, std::index_sequence_for<Ts...>{}, std::forward<Fn>(fn), access...);

        int index = static_cast<int>(nodes_.size());
        nodes_.push_back(std::move(node));
        compiled_ = false;

        return Task<void>(nullptr, detail::Graph_node_ref{ this, index, &link_edge });
    }

    // Resolve access conflicts + explicit edges into a DAG; detect cycles.
    void compile();

    // Run the compiled graph; returns a completion handle. Re-runnable.
    Task<void> execute(Scheduler& scheduler = default_scheduler());

private:
    struct Node
    {
        std::move_only_function<void()> run;
        std::vector<std::pair<const void*, Access>> access;
        std::vector<int> successors;
        int indegree = 0;
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
    static void link_edge(void* graph, int prerequisite, int successor);
    static bool conflicts(const Node& a, const Node& b);
    void detect_cycles() const;
    static void run_node(const std::shared_ptr<Run_state>& run, int index);

    std::vector<Node> nodes_;
    std::vector<std::pair<int, int>> explicit_edges_;
    bool compiled_ = false;
};

} // namespace ts
