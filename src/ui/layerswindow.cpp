#include "layerswindow.h"

#include "usdscene.h"

#include <imgui.h>

void drawLayersWindow(bool& show, bool editingBlocked, USDScene& usdScene, std::vector<SceneEditCommand>& pendingEdits) {
    if (!show || !usdScene.isOpen()) {
        return;
    }

    ImGui::Begin("Layers", &show);
    if (editingBlocked) {
        ImGui::TextDisabled("Scene updating...");
    } else {
        float bottomHeight = ImGui::GetFrameHeightWithSpacing() * 3;
        if (ImGui::BeginChild("##layerlist", {0, -bottomHeight}, ImGuiChildFlags_Borders)) {
            auto drawLayerRow = [&](const SceneLayerInfo& layer) {
                auto isCurrent = (layer.handle == usdScene.currentEditTarget());
                bool canMute = (layer.role == SceneLayerRole::Sublayer || layer.role == SceneLayerRole::Referenced);
                bool canEdit = (layer.role != SceneLayerRole::Referenced);

                ImGui::PushID(layer.handle.index);

                if (canMute) {
                    bool active = !layer.muted;
                    if (ImGui::Checkbox("##mute", &active)) {
                        pendingEdits.push_back({.type = SceneEditCommand::Type::MuteLayer, .layer = layer.handle, .boolValue = !active});
                    }
                    ImGui::SameLine();
                }

                std::string label = layer.displayName;
                if (layer.dirty) {
                    label += " (dirty)";
                }
                if (layer.readOnly) {
                    label += " [read-only]";
                }

                if (layer.muted) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Selectable(label.c_str(), isCurrent && canEdit)) {
                    if (canEdit) {
                        usdScene.setEditTarget(layer.handle);
                    }
                }
                if (layer.muted) {
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            };

            struct {
                SceneLayerRole role;
                const char* label;
            } sections[] = {
                {SceneLayerRole::Session, "Session"},
                {SceneLayerRole::Root, "Root"},
                {SceneLayerRole::Sublayer, "Sublayers"},
                {SceneLayerRole::Referenced, "Referenced"},
            };

            for (const auto& [role, sectionName] : sections) {
                bool hasAny = false;
                for (const auto& layer : usdScene.layers()) {
                    if (layer.role == role) {
                        hasAny = true;
                        break;
                    }
                }
                if (!hasAny) {
                    continue;
                }

                if (ImGui::TreeNodeEx(sectionName, ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (const auto& layer : usdScene.layers()) {
                        if (layer.role == role) {
                            drawLayerRow(layer);
                        }
                    }
                    ImGui::TreePop();
                }
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        static char newLayerPath[256] = "new_layer.usda";
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Add").x - ImGui::GetStyle().ItemSpacing.x -
                                ImGui::GetStyle().FramePadding.x * 2);
        ImGui::InputText("##newlayer", newLayerPath, sizeof(newLayerPath));
        ImGui::SameLine();
        if (ImGui::Button("Add")) {
            pendingEdits.push_back({.type = SceneEditCommand::Type::AddSubLayer, .stringValue = newLayerPath});
        }

        float buttonWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Save All", {buttonWidth, 0})) {
            usdScene.saveAllDirty();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Session", {buttonWidth, 0})) {
            pendingEdits.push_back({.type = SceneEditCommand::Type::ClearSession});
        }
    }
    ImGui::End();
}
