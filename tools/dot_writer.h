#pragma once

#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace ts::tools
{

// Minimal Graphviz DOT emitter for the graph structure dump
// (`Static_task_graph::compile(dot_path)`). Collects nodes and edges, then writes a
// `digraph` with a fixed dark style scheme (monokai-derived): solid pink = explicit
// ordering (`after`/`before`), dashed green = derived from a declared-access conflict;
// a graph label carries the legend. Render with Graphviz (`dot -Tsvg file.dot -o
// file.svg`, or `show_graph.bat`) or any online viewer.
class Dot_writer
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
        out += "digraph task_graph\n{\n";
        out += "    rankdir=LR;\n";
        out += "    bgcolor=\"#272822\";\n";
        out += "    node [shape=box, style=\"rounded,filled\", fillcolor=\"#3e3d32\", "
               "color=\"#66d9ef\", fontcolor=\"#f8f8f2\"];\n";
        out += "    label=\"solid pink = explicit ordering (after/before)"
               "\\ldashed green = derived from a declared-access conflict (hover for detail)\\l\";\n";
        out += "    labelloc=b;\n";
        out += "    fontsize=10;\n";
        out += "    fontcolor=\"#cfcfc2\";\n";

        for (const Node_entry& n : nodes_)
        {
            out += "    n" + std::to_string(n.id) + " [label=\"";
            append_escaped(out, n.label);
            out += "\"];\n";
        }

        for (const Edge_entry& e : edges_)
        {
            out += "    n" + std::to_string(e.from) + " -> n" + std::to_string(e.to);
            std::string attrs = (e.kind == Edge_kind::derived)
                ? "style=dashed, color=\"#a6e22e\", penwidth=1.8"
                : "color=\"#f92672\", penwidth=2.6";
            if (!e.tooltip.empty())
            {
                // `href` is required for Graphviz to emit the tooltip into SVG output.
                attrs += ", tooltip=\"";
                append_escaped(attrs, e.tooltip);
                attrs += "\", href=\"#\"";
            }
            out += " [" + attrs + "];\n";
        }

        out += "}\n";

        std::ofstream file(path, std::ios::binary);
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!file)
        {
            std::fprintf(stderr, "Dot_writer: cannot write '%s'\n", path);
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
