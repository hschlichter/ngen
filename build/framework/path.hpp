#pragma once

#include <filesystem>
#include <string>
#include <utility>

namespace build {

struct Path {
    std::filesystem::path value;

    Path() = default;
    Path(const char* path) : value(path) {}
    Path(std::string path) : value(std::move(path)) {}
    Path(std::filesystem::path path) : value(std::move(path)) {}

    std::string string() const { return value.generic_string(); }
    Path filename() const { return value.filename(); }
    Path parent_path() const { return value.parent_path(); }
    bool empty() const { return value.empty(); }
};

inline Path operator/(const Path& lhs, const Path& rhs) {
    return lhs.value / rhs.value;
}

inline bool operator<(const Path& lhs, const Path& rhs) {
    return lhs.string() < rhs.string();
}

} // namespace build
