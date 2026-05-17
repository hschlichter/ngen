// build::ir::dump_json — render an IR as human-readable JSON for `--dump-graph`.
//
// One JSON object per IR, including format version, variant string, project root, pools, every edge with all
// its fields, and the default target indices. Output is deterministic so diffs between runs are meaningful.
//
// Strictly a debugging affordance — not a parse target, no round-trip support, no equivalent reader. The graph
// stage emits this when the user runs `ngen-build --dump-graph`; nothing else consumes it.

#pragma once

#include "schema.hpp"

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace build::ir {

namespace detail {

inline auto json_escape(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size() + 2);
    for (char ch : s) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(ch));
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

inline auto write_string_array(std::ostream& out, const std::vector<std::string>& values, std::string_view indent) -> void {
    if (values.empty()) {
        out << "[]";
        return;
    }
    out << "[\n";
    for (std::size_t i = 0; i < values.size(); ++i) {
        out << indent << "  \"" << json_escape(values[i]) << "\"";
        if (i + 1 < values.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << indent << "]";
}

} // namespace detail

inline auto dump_json(const IR& ir, std::ostream& out) -> void {
    using detail::json_escape;
    using detail::write_string_array;

    out << "{\n";
    out << "  \"format_version\": " << kFormatVersion << ",\n";
    out << "  \"variant\": \"" << json_escape(ir.variant) << "\",\n";
    out << "  \"project_root\": \"" << json_escape(ir.project_root) << "\",\n";

    out << "  \"pools\": [";
    for (std::size_t i = 0; i < ir.pools.size(); ++i) {
        const auto& p = ir.pools[i];
        out << (i == 0 ? "\n" : "");
        out << "    {\"name\": \"" << json_escape(p.name) << "\", \"depth\": " << p.depth << "}";
        if (i + 1 < ir.pools.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";

    out << "  \"edges\": [";
    for (std::size_t i = 0; i < ir.edges.size(); ++i) {
        const auto& e = ir.edges[i];
        out << (i == 0 ? "\n" : "");
        out << "    {\n";
        out << "      \"name\": \"" << json_escape(e.name) << "\",\n";
        out << "      \"command\": \"" << json_escape(e.command) << "\",\n";
        out << "      \"inputs\": ";
        write_string_array(out, e.inputs, "      ");
        out << ",\n";
        out << "      \"outputs\": ";
        write_string_array(out, e.outputs, "      ");
        out << ",\n";
        out << "      \"implicit_deps\": ";
        write_string_array(out, e.implicit_deps, "      ");
        out << ",\n";
        out << "      \"order_only_deps\": ";
        write_string_array(out, e.order_only_deps, "      ");
        out << ",\n";
        out << "      \"pool\": " << e.pool << ",\n";
        out << "      \"depfile\": \"" << json_escape(e.depfile) << "\",\n";
        out << "      \"description\": \"" << json_escape(e.description) << "\",\n";
        out << "      \"flags\": " << e.flags << "\n";
        out << "    }";
        if (i + 1 < ir.edges.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";

    out << "  \"default_targets\": [";
    for (std::size_t i = 0; i < ir.default_targets.size(); ++i) {
        out << (i == 0 ? "" : ", ") << ir.default_targets[i];
    }
    out << "]\n";
    out << "}\n";
}

} // namespace build::ir
