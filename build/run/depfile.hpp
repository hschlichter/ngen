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

// Parse a Make-format depfile (the output of `-MMD -MF $out.d`). Returns the
// list of dependency paths after the `:`. Handles \-line continuations and
// \space escapes; treats # as a line comment.
//
// Example input (a backslash at end of line continues to the next):
//     foo.o: foo.c bar.h <BACKSLASH>
//            baz.h
// Returns: { "foo.c", "bar.h", "baz.h" }.
//
// The "target" before `:` is dropped. If the file is missing or empty, returns
// an empty list — that is the steady state for edges that don't produce a depfile.
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
