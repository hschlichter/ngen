#include "scenewindow.h"

#include "renderworld.h"

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <unordered_set>

namespace {

struct AddChildChoice {
    const char* label;
    const char* typeName;  // USD schema type name; fed to DefinePrim
    const char* baseName;  // unique-name base ("SphereLight", etc.)
};

// Typed-prim entries shown under the Add Child submenu. Lights are in display
// order; geometry entries author valid USD shape schemas even if Phase B hasn't
// tessellated them yet (they just won't render until it lands).
constexpr AddChildChoice kAddChildXform      = {"Xform",          "Xform",         "Xform"};
constexpr AddChildChoice kAddChildLights[]   = {
    {"Distant Light",  "DistantLight",  "DistantLight"},
    {"Sphere Light",   "SphereLight",   "SphereLight"},
    {"Rect Light",     "RectLight",     "RectLight"},
    {"Disk Light",     "DiskLight",     "DiskLight"},
    {"Cylinder Light", "CylinderLight", "CylinderLight"},
    {"Dome Light",     "DomeLight",     "DomeLight"},
};
constexpr AddChildChoice kAddChildGeometry[] = {
    {"Cube",     "Cube",     "Cube"},
    {"Sphere",   "Sphere",   "Sphere"},
    {"Cylinder", "Cylinder", "Cylinder"},
    {"Cone",     "Cone",     "Cone"},
};

// Derive a USD-valid prim name from a file stem — used as the default child
// name when the user creates a reference prim from an asset file. Replaces any
// character that isn't a letter / digit / underscore with an underscore; if the
// name starts with a digit, prepends one to keep it a valid identifier.
std::string sanitizeIdentifier(std::string_view stem) {
    std::string out;
    out.reserve(stem.size());
    for (char c : stem) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out.push_back(ok ? c : '_');
    }
    if (out.empty() || (out[0] >= '0' && out[0] <= '9')) {
        out.insert(out.begin(), '_');
    }
    return out;
}

} // namespace

void drawSceneWindow(bool& show,
                    bool editingBlocked,
                    USDScene& usdScene,
                    const RenderWorld& renderWorld,
                    PrimHandle& selectedPrim,
                    SceneWindowState& state,
                    std::vector<SceneEditCommand>& pendingEdits) {
    if (!show || !usdScene.isOpen()) {
        return;
    }

    // Only scroll to the selected node when the selection changed externally
    // (viewport pick, R = select parent, etc.) — not on every frame, otherwise
    // the user couldn't scroll the tree freely.
    bool scrollToSelected = (selectedPrim != state.lastSelectedPrim) && (bool) selectedPrim;
    state.lastSelectedPrim = selectedPrim;

    auto pushCreate = [&](const std::string& parentPath, const AddChildChoice& choice) {
        auto name = usdScene.uniqueChildName(parentPath.c_str(), choice.baseName);
        SceneEditCommand cmd{};
        cmd.type = SceneEditCommand::Type::CreatePrim;
        cmd.parentPath = parentPath;
        cmd.primName = std::move(name);
        cmd.typeName = choice.typeName;
        pendingEdits.push_back(std::move(cmd));
    };

    ImGui::Begin("Scene", &show);
    if (editingBlocked) {
        ImGui::TextDisabled("Scene updating...");
    } else {
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
                ImGui::Separator();
                if (ImGui::BeginMenu("Add Child")) {
                    if (ImGui::MenuItem(kAddChildXform.label)) {
                        pushCreate(rec->path, kAddChildXform);
                    }
                    ImGui::Separator();
                    ImGui::TextDisabled("Lights");
                    for (const auto& choice : kAddChildLights) {
                        if (ImGui::MenuItem(choice.label)) {
                            pushCreate(rec->path, choice);
                        }
                    }
                    ImGui::Separator();
                    ImGui::TextDisabled("Geometry");
                    for (const auto& choice : kAddChildGeometry) {
                        if (ImGui::MenuItem(choice.label)) {
                            pushCreate(rec->path, choice);
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Reference...")) {
                        state.openReferenceDialog = true;
                        state.referenceParent = rec->path;
                        state.referenceBrowser.rootDir = usdScene.rootLayerDirectory();
                        state.referenceBrowser.selected.clear();
                        invalidateAssetBrowser(state.referenceBrowser);
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Delete", "Del")) {
                    SceneEditCommand cmd{};
                    cmd.type = SceneEditCommand::Type::RemovePrim;
                    cmd.prim = h;
                    // Also record parent/name so undo-replay can re-resolve by path.
                    if (rec->parent) {
                        const auto* parentRec = usdScene.getPrimRecord(rec->parent);
                        if (parentRec) {
                            cmd.parentPath = parentRec->path;
                        }
                    }
                    cmd.primName = rec->name;
                    pendingEdits.push_back(std::move(cmd));
                    if (selectedPrim == h) {
                        selectedPrim = {};
                    }
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

    // Reference modal — opened by an Add Child → Reference… menu click. The
    // one-shot flag is consumed on the frame we OpenPopup, so repeated menu
    // hits reopen cleanly.
    if (state.openReferenceDialog) {
        ImGui::OpenPopup("Add Reference");
        state.openReferenceDialog = false;
    }
    ImGui::SetNextWindowSize({520, 460}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Add Reference", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextDisabled("Parent: %s", state.referenceParent.c_str());
        ImGui::Separator();

        float reserve = ImGui::GetFrameHeightWithSpacing() * 3.0f;
        float listH = ImGui::GetContentRegionAvail().y - reserve;
        drawAssetBrowser(state.referenceBrowser, std::max(listH, 100.0f));

        bool canCreate = !state.referenceBrowser.selected.empty();
        if (!canCreate) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Create")) {
            auto absPath = std::filesystem::path(state.referenceBrowser.rootDir) / state.referenceBrowser.selected;
            auto stem = absPath.stem().string();
            auto baseName = sanitizeIdentifier(stem);
            auto name = usdScene.uniqueChildName(state.referenceParent.c_str(), baseName.c_str());

            SceneEditCommand cmd{};
            cmd.type = SceneEditCommand::Type::CreateReferencePrim;
            cmd.parentPath = state.referenceParent;
            cmd.primName = std::move(name);
            cmd.referenceAsset = absPath.string();
            pendingEdits.push_back(std::move(cmd));
            ImGui::CloseCurrentPopup();
        }
        if (!canCreate) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}
