#include "propertieswindow.h"

#include "material.h"
#include "scenequery.h"
#include "usdscene.h"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

#include <cstdarg>
#include <cstdio>

// DragFloat3 with a left-aligned property label and per-axis X/Y/Z colored tags
// before each component. Sets `justActivated` and `committed` based on whichever
// sub-drag the user touched this frame — the caller uses them to drive undo
// capture and commit.
static bool axisDragFloat3(const char* label, float* values, float speed, bool& justActivated, bool& committed) {
    bool changed = false;
    ImGui::PushID(label);

    const char* axes[3] = {"X", "Y", "Z"};
    const ImU32 tags[3] = {IM_COL32(180, 70, 70, 255), IM_COL32(70, 150, 70, 255), IM_COL32(70, 110, 200, 255)};

    auto spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    auto tagW = ImGui::GetFrameHeight();
    auto labelColumnW = 90.0f;

    if (label[0] != '\0') {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelColumnW);
    }

    auto avail = ImGui::GetContentRegionAvail().x;
    auto dragW = (avail - tagW * 3.0f - spacing * 5.0f) / 3.0f;

    for (int i = 0; i < 3; i++) {
        if (i > 0) {
            ImGui::SameLine(0, spacing);
        }
        ImGui::PushStyleColor(ImGuiCol_Button, tags[i]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tags[i]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, tags[i]);
        ImGui::Button(axes[i], ImVec2(tagW, 0));
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0, spacing);
        ImGui::SetNextItemWidth(dragW);
        ImGui::PushID(i);
        if (ImGui::DragFloat("##v", &values[i], speed)) {
            changed = true;
        }
        justActivated |= ImGui::IsItemActivated();
        committed |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::PopID();
    }

    ImGui::PopID();
    return changed;
}

// Render a line of text that the user can click into, drag-select, and Ctrl+C
// from. Looks like ImGui::Text (no frame/padding) but is a read-only input.
static void selectableText(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushID(fmt); // disambiguate by call-site format string
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
    ImGui::PopID();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void drawPropertiesWindow(bool& show,
                          bool editingBlocked,
                          USDScene& usdScene,
                          PrimHandle selectedPrim,
                          const SceneQuerySystem& sceneQuery,
                          const MaterialLibrary& matLib,
                          std::vector<SceneEditCommand>& pendingEdits,
                          PropertiesWindowState& state) {
    if (!show) {
        return;
    }

    ImGui::Begin("Properties", &show);
    if (editingBlocked) {
        ImGui::TextDisabled("Scene updating...");
    } else {
        const auto* rec = selectedPrim ? usdScene.getPrimRecord(selectedPrim) : nullptr;
        if (rec) {
            selectableText("%s", rec->path.c_str());
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
                selectableText("Flags: %s", typeStr.c_str());
                selectableText("Active: %s", rec->active ? "yes" : "no");
                selectableText("Loaded: %s", rec->loaded ? "yes" : "no");
                if (rec->parent) {
                    const auto* parentRec = usdScene.getPrimRecord(rec->parent);
                    selectableText("Parent: %s", parentRec ? parentRec->path.c_str() : "<missing>");
                } else {
                    selectableText("Parent: %s", "(none — top-level)");
                }
                ImGui::TreePop();
            }

            const auto* xf = usdScene.getTransform(selectedPrim);
            if (xf && ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto worldPos = glm::vec3(xf->world[3]);
                ImGui::BeginDisabled();
                bool dummyA = false, dummyC = false;
                axisDragFloat3("World Pos", &worldPos.x, 0.1f, dummyA, dummyC);
                ImGui::EndDisabled();

                ImGui::Separator();

                auto local = xf->local;
                bool changed = false;
                bool committed = false;
                bool justActivated = false;

                changed |= axisDragFloat3("Position", &local.position.x, 0.1f, justActivated, committed);

                auto euler = glm::degrees(glm::eulerAngles(local.rotation));
                if (axisDragFloat3("Rotation", &euler.x, 0.5f, justActivated, committed)) {
                    local.rotation = glm::quat(glm::radians(euler));
                    changed = true;
                }

                changed |= axisDragFloat3("Scale", &local.scale.x, 0.01f, justActivated, committed);

                // Capture the pre-edit transform on the frame any slider becomes
                // active; remember it across frames (in `state`) until commit so
                // the undo stack records the *operation start* state, not the
                // post-Preview cache value (which has already been mutated by
                // per-frame Previews fired during the drag).
                if (justActivated && (!state.preEditLocal || state.preEditPrim != selectedPrim)) {
                    state.preEditLocal = local; // captured BEFORE this frame's Preview applies
                    state.preEditPrim = selectedPrim;
                }

                // Preview while the user is dragging the slider; commit one
                // Authoring edit on release. Same pattern as the translate gizmo.
                if (changed) {
                    pendingEdits.push_back({.type = SceneEditCommand::Type::SetTransform,
                                            .prim = selectedPrim,
                                            .transform = local,
                                            .purpose = SceneEditRequestContext::Purpose::Preview});
                }
                if (committed) {
                    pendingEdits.push_back(
                        {.type = SceneEditCommand::Type::SetTransform, .prim = selectedPrim, .transform = local, .inverseTransform = state.preEditLocal});
                    state.preEditLocal.reset();
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
            if (ImGui::TreeNodeEx("Bounds", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (bc && bc->valid) {
                    selectableText("Local min: %.2f, %.2f, %.2f", bc->localBounds.min.x, bc->localBounds.min.y, bc->localBounds.min.z);
                    selectableText("Local max: %.2f, %.2f, %.2f", bc->localBounds.max.x, bc->localBounds.max.y, bc->localBounds.max.z);
                    selectableText("World min: %.2f, %.2f, %.2f", bc->worldBounds.min.x, bc->worldBounds.min.y, bc->worldBounds.min.z);
                    selectableText("World max: %.2f, %.2f, %.2f", bc->worldBounds.max.x, bc->worldBounds.max.y, bc->worldBounds.max.z);
                } else {
                    ImGui::TextDisabled("(no own bounds)");
                }
                auto sub = sceneQuery.subtreeBounds(usdScene, selectedPrim);
                if (sub.valid()) {
                    auto c = (sub.min + sub.max) * 0.5f;
                    selectableText("Subtree min:  %.2f, %.2f, %.2f", sub.min.x, sub.min.y, sub.min.z);
                    selectableText("Subtree max:  %.2f, %.2f, %.2f", sub.max.x, sub.max.y, sub.max.z);
                    selectableText("Subtree ctr:  %.2f, %.2f, %.2f", c.x, c.y, c.z);
                } else {
                    ImGui::TextDisabled("(no subtree bounds)");
                }
                ImGui::TreePop();
            }

            // Display Color section — for any renderable prim (UsdGeomGprim). Authors
            // primvars:displayColor; the extractor re-tessellates on commit so the change
            // actually shows up. Shown in addition to the UsdShade material info below for
            // imported assets that have both.
            if (rec->flags & PrimFlagRenderable) {
                if (ImGui::TreeNodeEx("Display Color", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // If we're mid-edit on this prim, show the cached in-flight value —
                    // otherwise reading from USD each frame would snap the picker back to
                    // the old color during the drag and clobber what ColorEdit3 wrote to
                    // the local. Selection change to a different prim drops the cache.
                    if (state.displayColorEdit && state.displayColorEditPrim != selectedPrim) {
                        state.displayColorEdit.reset();
                    }
                    glm::vec3 editing = state.displayColorEdit.value_or(usdScene.getDisplayColor(selectedPrim));

                    bool changed = ImGui::ColorEdit3("Color", &editing.r);
                    if (changed) {
                        state.displayColorEdit = editing;
                        state.displayColorEditPrim = selectedPrim;
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit() && state.displayColorEdit) {
                        SceneEditCommand cmd{};
                        cmd.type = SceneEditCommand::Type::SetDisplayColor;
                        cmd.prim = selectedPrim;
                        cmd.colorValue = *state.displayColorEdit;
                        pendingEdits.push_back(std::move(cmd));
                        state.displayColorEdit.reset();
                    }
                    ImGui::TreePop();
                }
            }

            const auto* binding = usdScene.getAssetBinding(selectedPrim);
            if (binding && binding->material) {
                const auto* mat = matLib.get(binding->material);
                if (mat && ImGui::TreeNodeEx("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // Read-only preview; editing the UsdShade material goes through its
                    // own (not-yet-built) flow. Display Color above is what drives per-prim
                    // tinting for procedural shapes.
                    auto baseColor = mat->baseColorFactor;
                    ImGui::BeginDisabled();
                    ImGui::ColorEdit4("Base Color", (float*) &baseColor, ImGuiColorEditFlags_NoInputs);
                    ImGui::EndDisabled();
                    if (mat->texWidth > 0) {
                        selectableText("Texture: %dx%d", mat->texWidth, mat->texHeight);
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
