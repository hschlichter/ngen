#pragma once

#include "command.hpp"
#include "path.hpp"

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace build {

inline auto shell_quote(const std::string& value) -> std::string {
    if (value.empty()) {
        return "''";
    }
    bool simple = true;
    for (char ch : value) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-' && ch != '.' && ch != '/' && ch != ':' && ch != '=' && ch != '$') {
            simple = false;
            break;
        }
    }
    if (simple) {
        return value;
    }
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

inline auto join_command(const Command& command) -> std::string {
    std::string out;
    for (const auto& token : command.argv) {
        if (!out.empty()) {
            out += ' ';
        }
        out += shell_quote(token);
    }
    return out;
}

inline auto ninja_escape_path(const Path& path) -> std::string {
    std::string out;
    for (char ch : path.string()) {
        if (ch == ' ' || ch == ':' || ch == '$') {
            out += '$';
        }
        out += ch;
    }
    return out;
}

inline auto split_ws(const std::string& text) -> std::vector<std::string> {
    std::istringstream in(text);
    std::vector<std::string> tokens;
    std::string token;
    while (in >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

} // namespace build
