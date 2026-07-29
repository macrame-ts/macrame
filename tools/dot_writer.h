#pragma once

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ts::tools
{

// Minimal Graphviz DOT emitter for the graph structure dump
// (`Static_task_graph::compile(DOT_path)`). Collects nodes, edges, and the guarded-object
// list, then writes a `digraph` with a fixed dark style scheme (monokai-derived): edge
// COLOUR carries provenance (green = explicit `after`/`before`, cyan = derived from
// declared access), a numbered multi-column table of all guarded objects sits above the
// graph (left-aligned), and each node's label carries its accesses as `name - X, Y`
// object numbers (read = green, write = red, matching the trace renderer's access
// colours). Render with Graphviz
// (`dot -Tsvg file.dot -o file.svg`, or `show_graph.bat`) or any online viewer.
class DOT_writer
{
public:
    enum class Edge_kind
    {
        explicit_ordering,
        derived
    };

    // The guarded-object list, in numbering order: badge index i on a node refers to
    // `names[i]` (rendered 1-based).
    void set_objects(std::vector<std::string> names)
    {
        objects_ = std::move(names);
    }

    // `badges` = (object index into the `set_objects` list, is_write) per declared access.
    void add_node(int id, std::string_view label,
                  std::vector<std::pair<int, bool>> badges = {})
    {
        nodes_.push_back({ id, std::string(label), std::move(badges) });
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
        // Same face as the trace SVG (Segoe UI); Graphviz falls back if absent.
        out += "    fontname=\"Segoe UI\";\n";
        // Node border neutral grey: cyan is the derived-edge colour now, a cyan border
        // would read as edge-coloured.
        out += "    node [shape=box, style=\"rounded,filled\", fillcolor=\"#3e3d32\", "
               "color=\"#75715e\", fontcolor=\"#f8f8f2\", fontname=\"Segoe UI\"];\n";
        out += "    edge [fontname=\"Segoe UI\"];\n";

        // The guarded-object list as the graph label: top, LEFT-aligned, a bordered table.
        // COLUMN-MAJOR numbering -- consecutive numbers run DOWN a column, so finding
        // "object 17" is a vertical scan of one column, not a hunt across rows. Badge
        // numbers on nodes refer to it (1-based).
        if (!objects_.empty())
        {
            out += "    labelloc=\"t\";\n";
            out += "    labeljust=\"l\";\n";
            constexpr size_t columns = 8;
            const size_t rows = (objects_.size() + columns - 1) / columns;
            out += "    label=<<TABLE BORDER=\"1\" COLOR=\"#75715e\" CELLBORDER=\"0\" "
                   "CELLSPACING=\"0\" CELLPADDING=\"3\">\n";
            out += "        <TR><TD COLSPAN=\"" + std::to_string(columns * 2)
                + "\" ALIGN=\"LEFT\"><FONT COLOR=\"#75715e\" POINT-SIZE=\"10\">guarded objects</FONT></TD></TR>\n";
            for (size_t r = 0; r < rows; ++r)
            {
                out += "        <TR>";
                for (size_t col = 0; col < columns; ++col)
                {
                    const size_t c = col * rows + r;   // column-major
                    if (c < objects_.size())
                    {
                        out += "<TD ALIGN=\"RIGHT\"><FONT COLOR=\"#e6db74\" POINT-SIZE=\"11\">"
                            + std::to_string(c + 1) + "</FONT></TD><TD ALIGN=\"LEFT\"><FONT COLOR=\"#cfcfc2\" POINT-SIZE=\"11\">";
                        append_html_escaped(out, objects_[c]);
                        out += "</FONT></TD>";
                    }
                    else
                        out += "<TD></TD><TD></TD>";
                }
                out += "</TR>\n";
            }
            out += "    </TABLE>>;\n";
        }

        for (const Node_entry& n : nodes_)
        {
            // Access list sorted by object number -- both the label and the tooltip.
            std::vector<std::pair<int, bool>> badges = n.badges;
            std::sort(badges.begin(), badges.end());
            out += "    n" + std::to_string(n.id);
            if (badges.empty())
            {
                out += " [label=\"";
                append_escaped(out, n.label);
                out += "\"";
            }
            else
            {
                // HTML label, one line: `name - X, Y, Z` where X/Y/Z are the accessed
                // objects' numbers coloured by mode (read green / write red, the trace
                // renderer's access colours).
                out += " [label=<";
                append_html_escaped(out, n.label);
                out += "<FONT COLOR=\"#75715e\"> - </FONT>";
                bool first = true;
                for (const auto& [object, write] : badges)
                {
                    if (!first)
                        out += "<FONT COLOR=\"#75715e\">, </FONT>";
                    first = false;
                    out += std::string("<FONT COLOR=\"") + (write ? "#ff5f45" : "#a6e22e")
                        + "\">" + std::to_string(object + 1) + "</FONT>";
                }
                out += ">";
            }
            // The tooltip: the label (without one, browsers fall back to the SVG <title>
            // element, which is the internal node id `n7`), then one line per declared
            // access -- `{guarded number}: {name} - {mode}`.
            out += ", tooltip=\"";
            append_escaped(out, n.label);
            for (const auto& [object, write] : badges)
            {
                out += "\\n";   // a literal \n in a DOT string: line break in the SVG title
                out += std::to_string(object + 1) + ": ";
                if (object >= 0 && object < static_cast<int>(objects_.size()))
                    append_escaped(out, objects_[static_cast<size_t>(object)]);
                out += write ? " - read/write" : " - read-only";
            }
            out += "\"];\n";
        }

        for (const Edge_entry& e : edges_)
        {
            out += "    n" + std::to_string(e.from) + " -> n" + std::to_string(e.to);
            // Colour carries provenance: green = explicit ordering, cyan = derived from
            // declared access. Both solid.
            std::string attrs = (e.kind == Edge_kind::derived)
                ? "color=\"#66d9ef\", penwidth=1.8"
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

        // Legend: a single borderless HTML-table node -- the sample arrows are coloured
        // &#8594; glyphs, so Graphviz cannot misplace or stretch legend rows the way
        // invisible-endpoint sample edges could.
        out += "    legend [shape=none, margin=0, tooltip=\" \", label=<\n";
        out += "        <TABLE BORDER=\"0\" CELLBORDER=\"0\" CELLSPACING=\"2\" CELLPADDING=\"2\">\n";
        out += "        <TR><TD ALIGN=\"LEFT\"><FONT COLOR=\"#a6e22e\" POINT-SIZE=\"14\">&#8594;</FONT>"
               "<FONT COLOR=\"#cfcfc2\" POINT-SIZE=\"10\"> explicit ordering (after/before)</FONT></TD></TR>\n";
        out += "        <TR><TD ALIGN=\"LEFT\"><FONT COLOR=\"#66d9ef\" POINT-SIZE=\"14\">&#8594;</FONT>"
               "<FONT COLOR=\"#cfcfc2\" POINT-SIZE=\"10\"> derived from declared access</FONT></TD></TR>\n";
        out += "        <TR><TD ALIGN=\"LEFT\"><FONT COLOR=\"#a6e22e\">1</FONT>"
               "<FONT COLOR=\"#cfcfc2\" POINT-SIZE=\"10\"> - read-only, </FONT><FONT COLOR=\"#ff5f45\">2</FONT>"
               "<FONT COLOR=\"#cfcfc2\" POINT-SIZE=\"10\"> - read/write</FONT></TD></TR>\n";
        out += "        </TABLE>>];\n";

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
        std::vector<std::pair<int, bool>> badges;   // (object index, is_write)
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

    // Escaping inside HTML-like labels: the HTML entity set, not backslashes.
    static void append_html_escaped(std::string& out, std::string_view text)
    {
        for (char c : text)
        {
            if (c == '&')
                out += "&amp;";
            else if (c == '<')
                out += "&lt;";
            else if (c == '>')
                out += "&gt;";
            else
                out.push_back(c);
        }
    }

    std::vector<std::string> objects_;
    std::vector<Node_entry> nodes_;
    std::vector<Edge_entry> edges_;
};

} // namespace ts::tools
