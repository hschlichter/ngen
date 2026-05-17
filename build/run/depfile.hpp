// Parse a Make-format depfile (the output of `-MMD -MF $out.d`).
//
// Returns the list of dependency paths after the first `:`. Handles `\`-line continuations (the common shape —
// clang produces these for long header lists), `\space` escapes (paths with spaces), and `#` comments. The
// "target" portion before `:` is discarded; only the deps matter to us.
//
// Called by `execute.hpp` after each compile edge runs successfully: the parsed paths become
// `LogEntry::discovered_headers` and get hashed on every subsequent dirty check. Missing or empty files return
// an empty list — that's the steady state for edges that don't produce a depfile.
//
// We don't reuse a make implementation here. The format is simple enough to parse directly, and we'd
// otherwise inherit too much (variable expansion, conditional includes) from a real make.

#pragma once

#include "../framework/glob.hpp"
#include "../framework/path.hpp"

#include <expected>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

namespace ngen::run {

inline auto parse_depfile(const build::Path& path) -> std::expected<std::vector<std::string>, build::Error> {
    std::ifstream in(path.string(), std::ios::binary);
    if (!in) {
        return std::vector<std::string>{};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();

    // First, join \-continuation lines: backslash + newline -> single space.
    std::string joined;
    joined.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\n') {
            joined += ' ';
            ++i;
            continue;
        }
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\r' && i + 2 < text.size() && text[i + 2] == '\n') {
            joined += ' ';
            i += 2;
            continue;
        }
        joined += text[i];
    }

    std::vector<std::string> deps;

    // Walk line by line, find the `:`, then tokenize the rest.
    std::size_t pos = 0;
    while (pos < joined.size()) {
        auto eol = joined.find('\n', pos);
        std::string line = joined.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? joined.size() : eol + 1;

        // Strip comments. Make depfiles rarely contain '#' in real paths from
        // compiler output, so a naive find is fine.
        if (auto hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }

        // Find the first unescaped colon to separate targets from deps.
        std::size_t colon = std::string::npos;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\' && i + 1 < line.size()) {
                ++i;
                continue;
            }
            if (line[i] == ':') {
                colon = i;
                break;
            }
        }
        if (colon == std::string::npos) {
            continue;
        }
        std::string rhs = line.substr(colon + 1);

        // Tokenize on whitespace, respecting \-escaped spaces.
        std::string token;
        auto flush = [&]() {
            if (!token.empty()) {
                deps.push_back(std::move(token));
                token.clear();
            }
        };
        for (std::size_t i = 0; i < rhs.size(); ++i) {
            char ch = rhs[i];
            if (ch == '\\' && i + 1 < rhs.size()) {
                token += rhs[i + 1];
                ++i;
                continue;
            }
            if (ch == ' ' || ch == '\t' || ch == '\r') {
                flush();
                continue;
            }
            token += ch;
        }
        flush();
    }

    return deps;
}

} // namespace ngen::run
