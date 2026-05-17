// build::Path — thin wrapper around std::filesystem::path.
//
// Every path in the build system is a `Path`. The wrapper exists so `string()` always renders forward-slash
// (`generic_string()`) — important for consistent edge output paths, depfile comparisons, and platform-agnostic
// rendering — and so the surface is reduced to just what the build actually uses: implicit construction from
// string/char*, `operator/`, `filename()`, `parent_path()`, `empty()`, `operator<`. Nothing else leaks out, which
// keeps call sites short.
//
// Used everywhere. Depends only on `<filesystem>`.

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
