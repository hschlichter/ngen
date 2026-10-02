#pragma once

#include <filesystem>
#include <string>

// Asset ids are relative paths with forward slashes ("assets/main_sponza/textures/brick.png"), relative to the asset server's working
// directory. The id for a path relative to `base` (an id's folder, or empty): joined and normalised. Empty for an absolute path, or one
// that climbs above the server's directory.
inline auto assetIdForPath(const std::filesystem::path& path, const std::filesystem::path& base = {}) -> std::string {
    if (path.is_absolute() || base.is_absolute()) {
        return {};
    }
    auto id = (base / path).lexically_normal();
    if (id.empty() || id == "." || *id.begin() == "..") {
        return {};
    }
    return id.generic_string();
}
