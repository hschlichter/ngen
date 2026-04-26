#pragma once

#include "command.hpp"
#include "flags.hpp"
#include "path.hpp"

#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace build {

inline std::string shell_quote(const std::string& value) {
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

inline std::string join_command(const Command& command) {
    std::string out;
    for (const auto& token : command.argv) {
        if (!out.empty()) {
            out += ' ';
        }
        out += shell_quote(token);
    }
    return out;
}

inline std::string ninja_escape_path(const Path& path) {
    std::string out;
    for (char ch : path.string()) {
        if (ch == ' ' || ch == ':' || ch == '$') {
            out += '$';
        }
        out += ch;
    }
    return out;
}

inline std::string opt_flag(OptLevel opt) {
    switch (opt) {
        case OptLevel::O0:
            return "-O0";
        case OptLevel::O1:
            return "-O1";
        case OptLevel::O2:
            return "-O2";
        case OptLevel::O3:
            return "-O3";
    }
    return "-O0";
}

inline std::vector<std::string> split_ws(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> tokens;
    std::string token;
    while (in >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

} // namespace build
