// build::ir::compile_command_entries — derive `compile_commands.json` entries from an IR.
//
// Pure function: walks an `IR`'s edges, identifies the C/C++ compile edges by inspecting their inputs
// (single source file with a `.cpp` / `.cc` / `.cxx` / `.c` suffix), and emits one JSON object per compile
// in `compile_commands.json` format: `{"directory":..., "file":..., "command":...}`. The caller wraps the
// list in a JSON array and decides where to write it.
//
// No I/O, no `Project` dependency, no knowledge of variants or merged files. The IR is the only input.

#pragma once

#include "edge_kind.hpp" // `is_compile_edge`
#include "json.hpp"      // pulls in `detail::json_escape`, shared with the --dump-graph path.
#include "schema.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace build::ir {

namespace detail {

inline auto format_compile_entry(const std::string& directory, const Edge& edge) -> std::string {
    std::string out = "{\"directory\":\"";
    out += json_escape(directory);
    out += "\",\"file\":\"";
    out += json_escape(edge.inputs.front());
    out += "\",\"command\":\"";
    out += json_escape(edge.command);
    out += "\"}";
    return out;
}

} // namespace detail

inline auto compile_command_entries(const IR& ir) -> std::vector<std::string> {
    std::vector<std::string> out;
    out.reserve(ir.edges.size());
    for (const auto& edge : ir.edges) {
        if (is_compile_edge(edge)) {
            out.push_back(detail::format_compile_entry(ir.project_root, edge));
        }
    }
    return out;
}

} // namespace build::ir
