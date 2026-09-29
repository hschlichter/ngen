// build::ir::is_compile_edge — structural predicate for "this edge is a C/C++ compilation".
//
// Used by both the compile-commands extractor and the fuzzy target resolver. The rule is: a compile edge has
// exactly one input and that input ends in a recognised C/C++ source extension. No emit-time flag, no depfile
// check — the IR carries no special "is_compile" marker.
//
// Pure inspection. No I/O, no Project dependency.

#pragma once

#include "schema.hpp"

#include <string_view>

namespace build::ir {

inline auto is_compile_edge(const Edge& edge) -> bool {
    if (edge.inputs.size() != 1) {
        return false;
    }
    std::string_view input = edge.inputs.front();
    constexpr std::string_view exts[] = {".cpp", ".cc", ".cxx", ".c"};
    for (auto ext : exts) {
        if (input.size() >= ext.size() && input.substr(input.size() - ext.size()) == ext) {
            return true;
        }
    }
    return false;
}

} // namespace build::ir
