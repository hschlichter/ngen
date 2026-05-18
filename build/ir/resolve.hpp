// build::ir::resolve_target — fuzzy match a user-supplied target query against an IR.
//
// Pure function. Given the IR and a query string, returns the list of edge names the runner should invoke.
// The resolution rule, in order:
//
//   1. **Exact match.** If any edge's `name` equals `query`, returns `{query}` and stops.
//   2. **ObjectFile basenames.** Otherwise, look at every ObjectFile edge (one C/C++ source input — see
//      `is_compile_edge` in `edge_kind.hpp`) and check whether the source's stem (filename minus extension)
//      contains the query as a case-insensitive substring. If any match, returns every matching edge's name.
//   3. **Non-ObjectFile names.** Otherwise, check every non-ObjectFile edge's `name` for the same
//      case-insensitive substring. If any match, returns every matching edge's name.
//   4. **No match.** Returns an empty vector. The caller passes the original query through to the runner,
//      which produces its own "unknown target" error.
//
// Case-insensitive throughout. No cap on the match count — typing a very short query that fans out to many
// hits builds all of them.
//
// Lives in `ir/` because it operates on an `IR` value. No file I/O, no Project dependency.

#pragma once

#include "edge_kind.hpp"
#include "schema.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace build::ir {

namespace detail {

inline auto to_lower(std::string_view s) -> std::string {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

inline auto icontains(std::string_view haystack, std::string_view needle_lower) -> bool {
    // Caller passes `needle_lower` already lowercased; we lowercase haystack on the fly for cheap matching.
    if (needle_lower.empty() || needle_lower.size() > haystack.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle_lower.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle_lower.size(); ++j) {
            auto ch = static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[i + j])));
            if (ch != needle_lower[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

inline auto source_stem(std::string_view input) -> std::string_view {
    auto slash = input.find_last_of('/');
    auto start = slash == std::string_view::npos ? 0u : slash + 1;
    auto dot = input.find_last_of('.');
    if (dot == std::string_view::npos || dot < start) {
        return input.substr(start);
    }
    return input.substr(start, dot - start);
}

} // namespace detail

inline auto resolve_target(const IR& ir, std::string_view query) -> std::vector<std::string> {
    // Tier 1: exact match.
    for (const auto& e : ir.edges) {
        if (e.name == query) {
            return {std::string(query)};
        }
    }

    const auto needle = detail::to_lower(query);

    // Tier 2: ObjectFile source stems.
    std::vector<std::string> objectfile_matches;
    for (const auto& e : ir.edges) {
        if (!is_compile_edge(e)) {
            continue;
        }
        auto stem = detail::source_stem(e.inputs.front());
        if (detail::icontains(stem, needle)) {
            objectfile_matches.push_back(e.name);
        }
    }
    if (!objectfile_matches.empty()) {
        return objectfile_matches;
    }

    // Tier 3: non-ObjectFile edge names.
    std::vector<std::string> name_matches;
    for (const auto& e : ir.edges) {
        if (is_compile_edge(e)) {
            continue;
        }
        if (detail::icontains(e.name, needle)) {
            name_matches.push_back(e.name);
        }
    }
    if (!name_matches.empty()) {
        return name_matches;
    }

    return {};
}

} // namespace build::ir
