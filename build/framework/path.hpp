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

    auto string() const -> std::string { return value.generic_string(); }
    auto filename() const -> Path { return value.filename(); }
    auto parent_path() const -> Path { return value.parent_path(); }
    auto empty() const -> bool { return value.empty(); }
};

inline auto operator/(const Path& lhs, const Path& rhs) -> Path {
    return lhs.value / rhs.value;
}

inline auto operator<(const Path& lhs, const Path& rhs) -> bool {
    return lhs.string() < rhs.string();
}

} // namespace build
