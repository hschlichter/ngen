#include "assetbrowserwindow.h"

#include "usdscene.h"

#include <imgui.h>

#include <filesystem>

void drawAssetBrowserWindow(bool& show, USDScene& usdScene, AssetBrowserState& browser, std::vector<SceneEditCommand>& pendingEdits) {
    if (!show) {
        return;
    }

    ImGui::SetNextWindowSize({480, 420}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Asset Browser", &show);

    if (!usdScene.isOpen()) {
        ImGui::TextDisabled("Open a scene to browse assets");
        ImGui::End();
        return;
    }

    // `drawAssetBrowser` renders the refresh row + file list; the `height` param applies
    // to the scrollable list only. Reserve two frame rows: one for its own refresh row,
    // one for our Add button below.
    float listHeight = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.0f;
    drawAssetBrowser(browser, std::max(listHeight, 100.0f));

    bool canAdd = !browser.selected.empty();
    if (!canAdd) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Add as Sublayer")) {
        // Browser paths are relative to the project root (CWD), but the sublayer gets
        // resolved relative to the stage's root layer. Feed USD an absolute path so the
        // sublayer composes correctly regardless of which scene is open.
        auto absolutePath = std::filesystem::path(browser.rootDir) / browser.selected;
        pendingEdits.push_back({.type = SceneEditCommand::Type::AddSubLayer, .stringValue = absolutePath.string()});
    }
    if (!canAdd) {
        ImGui::EndDisabled();
    }

    ImGui::End();
}
