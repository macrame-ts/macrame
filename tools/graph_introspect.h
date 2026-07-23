#pragma once

// The single tools include for `static_task_graph.cpp`: the capture class
// (`Trace_stamps`, both configurations) plus the structure consumers below. All
// `TS_PROFILING` gating happens in here -- the graph calls the consumers
// unconditionally, and under `TS_PROFILING=0` they are empty stubs.

#include "trace_stamps.h"

#include <utility>
#include <vector>

#if TS_PROFILING

#include "dot_writer.h"
#include "graph_trace.h"

#include "ts/access.h"
#include "ts/priority.h"

#include <map>
#include <set>
#include <source_location>
#include <string>

namespace ts::tools
{

// Structure introspection over a compiled `Static_task_graph`, shared by the DOT dump and
// the trace structure push (and future consumers, e.g. ambiguity reporting). Function
// templates over the graph's private `Node` type: access control restricts naming a type,
// not deducing it, so the graph passes its `nodes_` from inside a member function and
// everything instantiates here, where `Pipe` and `Access` are complete. A `Node` provides
// `name`/`name_site`/`has_name_site`, `access` (instance pointer + `Access` pairs),
// `pipes` (parallel to `access`), and `priority`.

// A node's display label: its `Node_name` literal, else the named add_node's call site
// (`file:line`, basename only), else `node<N>`.
template<typename Node>
std::string node_label(const Node& node, int index)
{
    if (node.name)
        return node.name;
    if (node.has_name_site)
    {
        std::string_view file = node.name_site.file_name();
        if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos)
            file.remove_prefix(pos + 1);
        return std::string(file) + ":" + std::to_string(node.name_site.line());
    }
    return "node" + std::to_string(index);
}

// Object labels: the owning pipe's `debug_name` (`ts::Named`) when set, else an `objN`
// ordinal in first-declaration order. `access` and `pipes` are parallel arrays (same
// argument order), so an instance's pipe is at the same position.
template<typename Nodes>
std::map<const void*, std::string> object_labels(const Nodes& nodes)
{
    std::map<const void*, std::string> label;
    for (const auto& node : nodes)
        for (size_t k = 0; k < node.access.size(); ++k)
        {
            const char* name = node.pipes[k]->debug_name;
            label.try_emplace(node.access[k].first,
                name ? std::string(name) : "obj" + std::to_string(label.size()));
        }
    return label;
}

// The conflict detail between two nodes: one "objN: X->Y" entry per shared instance with
// at least one writer (empty = no conflict).
template<typename Node>
std::string conflict_detail(const Node& a, const Node& b, std::map<const void*, std::string>& label)
{
    std::string detail;
    for (const auto& [instance_a, mode_a] : a.access)
        for (const auto& [instance_b, mode_b] : b.access)
            if (instance_a == instance_b
                && (mode_a == Access::read_write || mode_b == Access::read_write))
            {
                if (!detail.empty())
                    detail += "; ";
                detail += label[instance_a] + ": ";
                detail += (mode_a == Access::read_write) ? 'W' : 'R';
                detail += "->";
                detail += (mode_b == Access::read_write) ? 'W' : 'R';
            }
    return detail;
}

// The one derivation walk. Visits every node -- `node_fn(index, label, accesses,
// priority)` where `accesses` is (object label, is_write) pairs -- then every deduped
// edge in compile()'s order -- `edge_fn(from, to, explicit_ordering, conflict_detail)`:
// explicit edges first (with the conflict detail of a coinciding conflict, possibly
// empty), then the remaining conflict-derived pairs (declaration-index order, detail
// always non-empty). Re-derives conflicts with detail -- an O(nodes^2) scan, paid only
// when a consumer runs; the compile path proper is untouched.
template<typename Nodes, typename NodeFn, typename EdgeFn>
void visit_structure(const Nodes& nodes, const std::vector<std::pair<int, int>>& explicit_edges,
                     NodeFn&& node_fn, EdgeFn&& edge_fn)
{
    std::map<const void*, std::string> labels = object_labels(nodes);

    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
    {
        std::vector<std::pair<std::string, bool>> accesses;
        for (const auto& [instance, mode] : nodes[i].access)
            accesses.emplace_back(labels[instance], mode == Access::read_write);
        node_fn(i, node_label(nodes[i], i), accesses, nodes[i].priority);
    }

    std::set<std::pair<int, int>> explicit_set(explicit_edges.begin(), explicit_edges.end());
    for (const auto& [from, to] : explicit_set)
        edge_fn(from, to, true, conflict_detail(nodes[from], nodes[to], labels));

    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
        for (int j = i + 1; j < static_cast<int>(nodes.size()); ++j)
        {
            if (explicit_set.contains({ i, j }))
                continue;
            std::string detail = conflict_detail(nodes[i], nodes[j], labels);
            if (!detail.empty())
                edge_fn(i, j, false, detail);
        }
}

// Consumer: the Graphviz DOT structure dump (`compile(dot_path)`). No-op on a null path.
template<typename Nodes>
void write_structure_dot(const Nodes& nodes, const std::vector<std::pair<int, int>>& explicit_edges,
                         const char* path)
{
    if (!path)
        return;
    Dot_writer dot;
    visit_structure(nodes, explicit_edges,
        [&dot](int i, const std::string& label, const auto&, Priority)
        {
            dot.add_node(i, label);   // the DOT dump does not render priority (yet)
        },
        [&dot](int from, int to, bool explicit_ordering, const std::string& detail)
        {
            // Every edge gets a tooltip: without one, browsers fall back to the SVG
            // <title> element, which is the internal edge id (`n11->n12`).
            if (explicit_ordering)
            {
                std::string tooltip = "explicit ordering";
                if (!detail.empty())
                    tooltip += "; " + detail;
                dot.add_edge(from, to, Dot_writer::Edge_kind::explicit_ordering, tooltip);
            }
            else
                dot.add_edge(from, to, Dot_writer::Edge_kind::derived, detail);
        });
    dot.dump(path);
}

// Consumer: the attached trace's structure (labels, priorities, declared accesses, edges
// with provenance). No-op on a null trace; resets the trace's aggregates
// (`begin_structure`).
template<typename Nodes>
void push_structure(Graph_trace* trace, const Nodes& nodes,
                    const std::vector<std::pair<int, int>>& explicit_edges)
{
    if (!trace)
        return;
    trace->begin_structure(static_cast<int>(nodes.size()));
    visit_structure(nodes, explicit_edges,
        [trace](int i, const std::string& label, const std::vector<std::pair<std::string, bool>>& accesses,
                Priority priority)
        {
            trace->set_node_label(i, label);
            trace->set_node_priority(i, priority);
            for (const auto& [object, write] : accesses)
                trace->add_node_access(i, object, write);
        },
        [trace](int from, int to, bool explicit_ordering, const std::string& detail)
        {
            trace->add_edge(from, to, explicit_ordering, detail);
        });
}

} // namespace ts::tools

#else // TS_PROFILING == 0: the graph still calls the consumers; they compile to nothing.

namespace ts::tools
{

template<typename Nodes>
void write_structure_dot(const Nodes&, const std::vector<std::pair<int, int>>&, const char*)
{
}

template<typename Nodes>
void push_structure(Graph_trace*, const Nodes&, const std::vector<std::pair<int, int>>&)
{
}

} // namespace ts::tools

#endif // TS_PROFILING
