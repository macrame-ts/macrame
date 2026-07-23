#pragma once

#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace ts::tools
{

// Minimal Graphviz DOT emitter for the graph structure dump
// (`Static_task_graph::compile(DOT_path)`). Collects nodes and edges, then writes a
// `digraph` with a fixed dark style scheme (monokai-derived): green edges, solid =
// explicit ordering (`after`/`before`), dashed = derived from declared access; a legend
// cluster shows the styles. Render with Graphviz (`dot -Tsvg file.dot -o file.svg`, or
// `show_graph.bat`) or any online viewer.
class DOT_writer
{
public:
    enum class Edge_kind
    {
        explicit_ordering,
        derived
    };

    void add_node(int id, std::string_view label)
    {
        nodes_.push_back({ id, std::string(label) });
    }

    // `tooltip` shows on hover in SVG output (conflict detail for a derived edge --
    // information without visual clutter).
    void add_edge(int from, int to, Edge_kind kind, std::string_view tooltip = {})
    {
        edges_.push_back({ from, to, kind, std::string(tooltip) });
    }

    // Write the graph; returns false (reported to stderr) on I/O failure.
    bool dump(const char* path) const
    {
        std::string out;
        // Anonymous digraph: a named one becomes the SVG root's <title>, which browsers
        // then show as a hover tooltip anywhere the cursor misses an edge or node --
        // confusing next to the real per-edge tooltips.
        out += "digraph\n{\n";
        out += "    rankdir=LR;\n";
        out += "    ranksep=0.25;\n";   // rank gap: sets edge length (incl. the legend sample arrows)
        out += "    bgcolor=\"#272822\";\n";
        out += "    node [shape=box, style=\"rounded,filled\", fillcolor=\"#3e3d32\", "
               "color=\"#66d9ef\", fontcolor=\"#f8f8f2\"];\n";

        for (const Node_entry& n : nodes_)
        {
            out += "    n" + std::to_string(n.id) + " [label=\"";
            append_escaped(out, n.label);
            // The tooltip repeats the label: without one, browsers fall back to the SVG
            // <title> element, which is the internal node id (`n7`).
            out += "\", tooltip=\"";
            append_escaped(out, n.label);
            out += "\"];\n";
        }

        for (const Edge_entry& e : edges_)
        {
            out += "    n" + std::to_string(e.from) + " -> n" + std::to_string(e.to);
            // Line style carries provenance (solid = explicit, dashed = derived); one
            // colour for both -- matches the trace renderer, where colour is reserved
            // for criticality.
            std::string attrs = (e.kind == Edge_kind::derived)
                ? "style=dashed, color=\"#a6e22e\", penwidth=1.8"
                : "color=\"#a6e22e\", penwidth=2.0";
            if (!e.tooltip.empty())
            {
                // `href` is required for Graphviz to emit the tooltip into SVG output.
                attrs += ", tooltip=\"";
                append_escaped(attrs, e.tooltip);
                attrs += "\", href=\"#\"";
            }
            out += " [" + attrs + "];\n";
        }

        // Legend: real styled edges between invisible endpoints, so the key shows the
        // arrows themselves instead of describing them.
        out += "    subgraph cluster_legend\n    {\n";
        out += "        label=\"legend\";\n";
        out += "        tooltip=\" \";\n";   // suppress the browser showing "cluster_legend" on hover
        out += "        fontsize=10;\n";
        out += "        fontcolor=\"#cfcfc2\";\n";
        out += "        color=\"#3e3d32\";\n";
        // Each row is a short unlabeled arrow into a text-only node, so the sample arrow
        // and its explanation sit on one line (an edge LABEL would stretch the arrow).
        out += "        node [shape=none, style=\"\", label=\"\", width=0.01, height=0.01, "
               "fontsize=10, fontcolor=\"#cfcfc2\"];\n";
        // Both text nodes share one fixed width with left-justified labels (`\l`), so
        // their left edges -- and therefore the sample arrows -- are the same length.
        out += "        l1 [label=\"explicit ordering (after/before)\\l\", width=3];\n";
        out += "        l3 [label=\"derived from declared access (hover for detail)\\l\", width=3];\n";
        out += "        l0 -> l1 [color=\"#a6e22e\", penwidth=2.0, tooltip=\"explicit ordering\"];\n";
        out += "        l2 -> l3 [style=dashed, color=\"#a6e22e\", penwidth=1.8, tooltip=\"derived edge\"];\n";
        out += "    }\n";

        out += "}\n";

        std::ofstream file(path, std::ios::binary);
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!file)
        {
            std::fprintf(stderr, "DOT_writer: cannot write '%s'\n", path);
            return false;
        }
        return true;
    }

private:
    struct Node_entry
    {
        int id;
        std::string label;
    };
    struct Edge_entry
    {
        int from;
        int to;
        Edge_kind kind;
        std::string tooltip;
    };

    static void append_escaped(std::string& out, std::string_view text)
    {
        for (char c : text)
        {
            if (c == '"' || c == '\\')
                out.push_back('\\');
            out.push_back(c);
        }
    }

    std::vector<Node_entry> nodes_;
    std::vector<Edge_entry> edges_;
};

} // namespace ts::tools
