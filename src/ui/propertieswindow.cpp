#include "propertieswindow.h"

#include "material.h"
#include "scenequery.h"
#include "usdscene.h"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

void drawPropertiesWindow(bool& show,
                          bool editingBlocked,
                          USDScene& usdScene,
                          PrimHandle selectedPrim,
                          const SceneQuerySystem& sceneQuery,
                          const MaterialLibrary& matLib,
                          std::vector<SceneEditCommand>& pendingEdits) {
    if (!show) {
        return;
    }

    ImGui::Begin("Properties", &show);
    if (editingBlocked) {
        ImGui::TextDisabled("Scene updating...");
    } else {
        const auto* rec = selectedPrim ? usdScene.getPrimRecord(selectedPrim) : nullptr;
        if (rec) {
            ImGui::Text("%s", rec->path.c_str());
            ImGui::Separator();

            if (ImGui::TreeNodeEx("Prim Info", ImGuiTreeNodeFlags_DefaultOpen)) {
                std::string typeStr;
                if (rec->flags & PrimFlagRenderable) {
                    typeStr += "Mesh ";
                }
                if (rec->flags & PrimFlagLight) {
                    typeStr += "Light ";
                }
                if (rec->flags & PrimFlagCamera) {
                    typeStr += "Camera ";
                }
                if (rec->flags & PrimFlagXformable) {
                    typeStr += "Xform ";
                }
                if (typeStr.empty()) {
                    typeStr = "None";
                }
                ImGui::Text("Flags: %s", typeStr.c_str());
                ImGui::Text("Active: %s", rec->active ? "yes" : "no");
                ImGui::Text("Loaded: %s", rec->loaded ? "yes" : "no");
                ImGui::TreePop();
            }

            const auto* xf = usdScene.getTransform(selectedPrim);
            if (xf && ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto worldPos = glm::vec3(xf->world[3]);
                ImGui::BeginDisabled();
                ImGui::DragFloat3("World Pos", &worldPos.x);
                ImGui::EndDisabled();

                ImGui::Separator();

                auto local = xf->local;
                bool changed = false;
                changed |= ImGui::DragFloat3("Position", &local.position.x, 0.1f);
                auto euler = glm::degrees(glm::eulerAngles(local.rotation));
                if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f)) {
                    local.rotation = glm::quat(glm::radians(euler));
                    changed = true;
                }
                changed |= ImGui::DragFloat3("Scale", &local.scale.x, 0.01f);
                if (changed) {
                    pendingEdits.push_back({.type = SceneEditCommand::Type::SetTransform, .prim = selectedPrim, .transform = local});
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNodeEx("Visibility", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool visible = rec->visible;
                if (ImGui::Checkbox("Visible", &visible)) {
                    pendingEdits.push_back({.type = SceneEditCommand::Type::SetVisibility, .prim = selectedPrim, .boolValue = visible});
                }
                ImGui::TreePop();
            }

            const auto* bc = sceneQuery.bounds().get(selectedPrim);
            if (bc && bc->valid && ImGui::TreeNodeEx("Bounds", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Local min: %.2f, %.2f, %.2f", bc->localBounds.min.x, bc->localBounds.min.y, bc->localBounds.min.z);
                ImGui::Text("Local max: %.2f, %.2f, %.2f", bc->localBounds.max.x, bc->localBounds.max.y, bc->localBounds.max.z);
                ImGui::Text("World min: %.2f, %.2f, %.2f", bc->worldBounds.min.x, bc->worldBounds.min.y, bc->worldBounds.min.z);
                ImGui::Text("World max: %.2f, %.2f, %.2f", bc->worldBounds.max.x, bc->worldBounds.max.y, bc->worldBounds.max.z);
                ImGui::TreePop();
            }

            const auto* binding = usdScene.getAssetBinding(selectedPrim);
            if (binding && binding->material) {
                const auto* mat = matLib.get(binding->material);
                if (mat && ImGui::TreeNodeEx("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::ColorEdit4("Base Color", (float*) &mat->baseColorFactor, ImGuiColorEditFlags_NoInputs);
                    if (mat->texWidth > 0) {
                        ImGui::Text("Texture: %dx%d", mat->texWidth, mat->texHeight);
                    } else {
                        ImGui::TextDisabled("No texture");
                    }
                    ImGui::TreePop();
                }
            }
        } else {
            ImGui::TextDisabled("No prim selected");
        }
    }
    ImGui::End();
}
