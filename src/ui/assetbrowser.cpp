#include "assetbrowser.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

// Extensions recognized as usd-family layers. Kept lowercase; comparison is case-insensitive.
static bool isUsdExtension(const fs::path& p) {
    auto ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz";
}

static void rescan(AssetBrowserState& state) {
    state.cachedFiles.clear();
    if (state.rootDir.empty()) {
        return;
    }
    std::error_code ec;
    auto rootPath = fs::path(state.rootDir);
    if (!fs::is_directory(rootPath, ec)) {
        return;
    }
    for (auto it = fs::recursive_directory_iterator(rootPath, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        if (!isUsdExtension(it->path())) {
            continue;
        }
        auto rel = fs::relative(it->path(), rootPath, ec);
        if (ec) {
            continue;
        }
        state.cachedFiles.push_back(rel.generic_string());
    }
    std::ranges::sort(state.cachedFiles);
}

bool drawAssetBrowser(AssetBrowserState& state, float height) {
    if (state.dirty) {
        rescan(state);
        state.dirty = false;
    }

    bool changed = false;

    if (ImGui::Button("Refresh")) {
        invalidateAssetBrowser(state);
    }
    ImGui::SameLine();
    if (state.rootDir.empty()) {
        ImGui::TextDisabled("(no scene open)");
    } else {
        ImGui::TextDisabled("%s", state.rootDir.c_str());
    }

    if (ImGui::BeginChild("##assetbrowserlist", {0, height}, ImGuiChildFlags_Borders)) {
        if (state.cachedFiles.empty()) {
            ImGui::TextDisabled("No .usd/.usda/.usdc/.usdz files found");
        } else {
            for (const auto& rel : state.cachedFiles) {
                bool isSelected = (rel == state.selected);
                if (ImGui::Selectable(rel.c_str(), isSelected)) {
                    if (state.selected != rel) {
                        state.selected = rel;
                        changed = true;
                    }
                }
            }
        }
    }
    ImGui::EndChild();

    return changed;
}

void invalidateAssetBrowser(AssetBrowserState& state) {
    state.dirty = true;
    state.cachedFiles.clear();
}
