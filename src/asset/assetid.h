#pragma once

#include <filesystem>
#include <string>

// Asset ids are project-relative paths with forward slashes ("assets/main_sponza/textures/brick.png"). The id for a path: a relative
// path is taken against `base` (the working directory, or the directory of the asset that refers to it), then made relative to the
// project root and normalised. Empty when the path is outside the project.
inline auto assetIdForPath(const std::filesystem::path& path, const std::filesystem::path& base, const std::filesystem::path& projectRoot) -> std::string {
    auto absolute = (path.is_absolute() ? path : base / path).lexically_normal();
    auto relative = absolute.lexically_relative(projectRoot.lexically_normal());
    if (relative.empty() || *relative.begin() == "..") {
        return {};
    }
    return relative.generic_string();
}
