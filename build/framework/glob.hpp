// Globbing, build-time string utilities, and the framework `Error` type.
//
// Three loosely related responsibilities share this file because every other framework header pulls it in
// transitively — the cheapest place to land utility code without inflating include graphs.
//
//   - `glob(GlobSpec)` walks the filesystem and returns matching paths, sorted. The matcher is a hand-rolled
//     recursive routine (see `detail::glob_match_view`) supporting `*`, `**`, `**/`, `?`. We deliberately do not
//     use `std::regex` — it throws, and the framework forbids exceptions.
//   - `shell_quote`, `split_ws`, `capture_tokens`, `repo_root`, `concat`, `concat_tokens`, `write_if_changed` —
//     small helpers used by the emitter, by tool substitution, and by command builders. `capture_tokens` is
//     `popen`-based and intended for fixed args like `pkg-config --cflags <lib>`; it is not safe for arbitrary
//     user input.
//   - `build::Error` — the framework's error type, threaded through every `std::expected<T, Error>` return.
//     Living here means callers that need `Error` don't pay an extra include.

#pragma once

#include "path.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace build {

struct Error {
    std::string message;
};

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

inline auto split_ws(const std::string& text) -> std::vector<std::string> {
    std::istringstream in(text);
    std::vector<std::string> tokens;
    std::string token;
    while (in >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

struct GlobSpec {
    std::string include;
    std::string exclude = "";
};

namespace detail {

// Recursive glob matcher. Supports:
//   *    — any sequence of non-'/' chars (single path segment)
//   **   — any sequence including '/' (multiple path segments)
//   **/  — zero or more path segments followed by '/'
//   ?    — any single non-'/' char
// All other characters match literally. No exceptions; std::regex is avoided
// so the matcher is safe to call from noexcept paths.
inline auto glob_match_view(std::string_view pattern, std::string_view text) -> bool {
    if (pattern.size() >= 3 && pattern.substr(0, 3) == "**/") {
        auto rest = pattern.substr(3);
        if (glob_match_view(rest, text)) {
            return true;
        }
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '/' && glob_match_view(rest, text.substr(i + 1))) {
                return true;
            }
        }
        return false;
    }
    if (pattern.size() >= 2 && pattern.substr(0, 2) == "**") {
        auto rest = pattern.substr(2);
        for (size_t i = 0; i <= text.size(); ++i) {
            if (glob_match_view(rest, text.substr(i))) {
                return true;
            }
        }
        return false;
    }
    if (!pattern.empty() && pattern[0] == '*') {
        auto rest = pattern.substr(1);
        size_t slash = text.find('/');
        size_t limit = (slash == std::string_view::npos) ? text.size() : slash;
        for (size_t i = 0; i <= limit; ++i) {
            if (glob_match_view(rest, text.substr(i))) {
                return true;
            }
        }
        return false;
    }
    if (pattern.empty()) {
        return text.empty();
    }
    if (text.empty()) {
        return false;
    }
    if (pattern[0] == '?') {
        if (text[0] == '/') {
            return false;
        }
        return glob_match_view(pattern.substr(1), text.substr(1));
    }
    if (pattern[0] != text[0]) {
        return false;
    }
    return glob_match_view(pattern.substr(1), text.substr(1));
}

inline auto glob_match(std::string pattern, std::string text) -> bool {
    std::ranges::replace(pattern, '\\', '/');
    std::ranges::replace(text, '\\', '/');
    return glob_match_view(pattern, text);
}

} // namespace detail

inline auto glob(GlobSpec spec) -> std::vector<Path> {
    std::vector<Path> out;
    auto root = std::filesystem::current_path();
    auto wildcard = spec.include.find_first_of("*?");
    std::filesystem::path search_root = root;
    if (wildcard != std::string::npos) {
        auto prefix = spec.include.substr(0, wildcard);
        auto slash = prefix.find_last_of('/');
        if (slash != std::string::npos) {
            search_root = root / prefix.substr(0, slash);
        }
    }
    if (!std::filesystem::exists(search_root)) {
        return out;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(search_root); it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) {
            continue;
        }
        auto rel = std::filesystem::relative(it->path(), root).generic_string();
        if (detail::glob_match(spec.include, rel) && (spec.exclude.empty() || !detail::glob_match(spec.exclude, rel))) {
            out.emplace_back(rel);
        }
    }
    std::ranges::sort(out, [](const Path& lhs, const Path& rhs) { return lhs.string() < rhs.string(); });
    return out;
}

inline auto concat(std::initializer_list<std::vector<Path>> lists) -> std::vector<Path> {
    std::vector<Path> out;
    for (const auto& list : lists) {
        out.insert(out.end(), list.begin(), list.end());
    }
    return out;
}

inline auto concat_tokens(std::initializer_list<std::vector<std::string>> lists) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& list : lists) {
        out.insert(out.end(), list.begin(), list.end());
    }
    return out;
}

inline auto concat_tokens(std::vector<std::string> a, std::vector<std::string> b) -> std::vector<std::string> {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

inline auto capture_tokens(std::initializer_list<std::string> argv) -> std::vector<std::string> {
    std::string command;
    for (const auto& token : argv) {
        if (!command.empty()) {
            command += ' ';
        }
        command += shell_quote(token);
    }
    std::array<char, 256> buffer{};
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return {};
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    auto rc = pclose(pipe);
    if (rc != 0) {
        return {};
    }
    return split_ws(output);
}

inline auto repo_root() -> std::string {
    return std::filesystem::current_path().string();
}

inline auto write_if_changed(const Path& path, const std::string& text) -> std::expected<void, Error> {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path().value, ec);
        if (ec) {
            return std::unexpected(Error{"failed to create directory " + path.parent_path().string() + ": " + ec.message()});
        }
    }
    {
        std::ifstream existing(path.string());
        std::ostringstream current;
        current << existing.rdbuf();
        if (existing && current.str() == text) {
            return {};
        }
    }
    std::ofstream out(path.string(), std::ios::binary);
    if (!out) {
        return std::unexpected(Error{"failed to open " + path.string() + " for writing"});
    }
    out << text;
    if (!out) {
        return std::unexpected(Error{"failed to write " + path.string()});
    }
    return {};
}

} // namespace build
