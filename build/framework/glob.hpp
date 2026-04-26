#pragma once

#include "flags.hpp"
#include "path.hpp"
#include "toolchainhelpers.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ranges>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace build {

struct GlobSpec {
    std::string include;
    std::string exclude = "";
};

namespace detail {

inline auto glob_match(std::string pattern, std::string text) -> bool {
    std::ranges::replace(pattern, '\\', '/');
    std::ranges::replace(text, '\\', '/');
    std::string re = "^";
    for (size_t i = 0; i < pattern.size(); ++i) {
        char ch = pattern[i];
        if (ch == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                if (i + 2 < pattern.size() && pattern[i + 2] == '/') {
                    re += "(?:.*/)?";
                    i += 2;
                } else {
                    re += ".*";
                    ++i;
                }
            } else {
                re += "[^/]*";
            }
        } else if (ch == '?') {
            re += "[^/]";
        } else {
            if (std::string_view(R"(\.^$|()[]{}+)").find(ch) != std::string_view::npos) {
                re += '\\';
            }
            re += ch;
        }
    }
    re += "$";
    return std::regex_match(text, std::regex(re));
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
