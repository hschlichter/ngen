#include "scenewindow.h"

#include "renderworld.h"

#include <imgui.h>

#include <functional>
#include <unordered_set>

void drawSceneWindow(bool& show, bool editingBlocked, USDScene& usdScene, const RenderWorld& renderWorld, PrimHandle& selectedPrim, SceneWindowState& state) {
    if (!show || !usdScene.isOpen()) {
        return;
    }

    // Only scroll to the selected node when the selection changed externally
    // (viewport pick, R = select parent, etc.) — not on every frame, otherwise
    // the user couldn't scroll the tree freely.
    bool scrollToSelected = (selectedPrim != state.lastSelectedPrim) && (bool) selectedPrim;
    state.lastSelectedPrim = selectedPrim;

    ImGui::Begin("Scene", &show);
    if (editingBlocked) {
        ImGui::TextDisabled("Scene updating...");
    } else {
        ImGui::Text("Mesh instances: %zu", renderWorld.meshInstances.size());

        if (selectedPrim) {
            const auto* rec = usdScene.getPrimRecord(selectedPrim);
            if (rec) {
                ImGui::Separator();
                ImGui::Text("Selected: %s", rec->path.c_str());
            }
        }

        std::unordered_set<uint32_t> selectedAncestors;
        if (selectedPrim) {
            auto cur = selectedPrim;
            while (cur) {
                const auto* r = usdScene.getPrimRecord(cur);
                if (!r || !r->parent) {
                    break;
                }
                cur = r->parent;
                selectedAncestors.insert(cur.index);
            }
        }

        std::function<void(PrimHandle)> drawSceneNode;
        drawSceneNode = [&](PrimHandle h) {
            const auto* rec = usdScene.getPrimRecord(h);
            if (!rec) {
                return;
            }

            bool hasChildren = static_cast<bool>(usdScene.firstChild(h));
            bool isSelected = (h == selectedPrim);

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (isSelected) {
                flags |= ImGuiTreeNodeFlags_Selected;
            }
            if (!hasChildren) {
                flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            }

            if (selectedAncestors.contains(h.index)) {
                ImGui::SetNextItemOpen(true);
            }

            const char* tag = "";
            if (rec->flags & PrimFlagRenderable) {
                tag = " [mesh]";
            } else if (rec->flags & PrimFlagLight) {
                tag = " [light]";
            }

            ImGui::PushID(h.index);
            bool open = ImGui::TreeNodeEx(rec->name.c_str(), flags, "%s%s", rec->name.c_str(), tag);

            if (isSelected && scrollToSelected) {
                ImGui::SetScrollHereY();
            }

            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                selectedPrim = isSelected ? PrimHandle{} : h;
            }

            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Copy")) {
                    ImGui::SetClipboardText(rec->name.c_str());
                }
                if (ImGui::MenuItem("Copy Full Path")) {
                    ImGui::SetClipboardText(rec->path.c_str());
                }
                ImGui::EndPopup();
            }

            if (open && hasChildren) {
                auto child = usdScene.firstChild(h);
                while (child) {
                    drawSceneNode(child);
                    child = usdScene.nextSibling(child);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        };

        if (ImGui::CollapsingHeader("Scene Graph", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& prim : usdScene.allPrims()) {
                if (!prim.parent) {
                    drawSceneNode(prim.handle);
                }
            }
        }
    }
    ImGui::End();
}
