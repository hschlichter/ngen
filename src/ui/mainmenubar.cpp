#include "mainmenubar.h"

#include "camera.h"
#include "scenequery.h"
#include "sceneupdater.h"
#include "undostack.h"
#include "usdscene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <imgui.h>

#include <SDL3/SDL.h>

void drawMainMenuBar(MainMenuBarState& state) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...")) {
                static const SDL_DialogFileFilter filters[] = {
                    {"USD Files", "usd;usda;usdc;usdz"},
                    {"All Files", "*"},
                };
                SDL_ShowOpenFileDialog(
                    [](void* userdata, const char* const* filelist, int) {
                        if (filelist && filelist[0]) {
                            *static_cast<std::string*>(userdata) = filelist[0];
                        }
                    },
                    &state.pendingOpenPath,
                    state.window,
                    filters,
                    2,
                    nullptr,
                    false);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) {
                state.requestQuit = true;
            }
            ImGui::EndMenu();
        }
        if (state.sceneUpdater && ImGui::BeginMenu("Edit")) {
            auto& stack = state.sceneUpdater->undoStack();
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, stack.canUndo())) {
                for (auto& c : stack.undo()) {
                    state.sceneUpdater->addEdit(std::move(c));
                }
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, stack.canRedo())) {
                for (auto& c : stack.redo()) {
                    state.sceneUpdater->addEdit(std::move(c));
                }
            }
            ImGui::Separator();

            bool hasSelection = state.selectedPrim && (bool) *state.selectedPrim;
            const auto* selRec = (hasSelection && state.usdScene) ? state.usdScene->getPrimRecord(*state.selectedPrim) : nullptr;
            bool hasParent = selRec && (bool) selRec->parent;
            if (ImGui::MenuItem("Select Parent", "R", false, hasParent)) {
                *state.selectedPrim = selRec->parent;
            }
            bool canFrame = hasSelection && state.sceneQuery && state.camera && state.usdScene;
            if (ImGui::MenuItem("Frame Selected", "F", false, canFrame)) {
                auto bb = state.sceneQuery->anchorBounds(*state.usdScene, *state.selectedPrim);
                if (bb.valid()) {
                    state.camera->frame(bb, glm::radians(45.0f));
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows")) {
            ImGui::MenuItem("Scene", nullptr, &state.showSceneWindow, state.sceneOpen);
            ImGui::MenuItem("Properties", nullptr, &state.showPropertiesWindow, state.sceneOpen);
            ImGui::MenuItem("Layers", nullptr, &state.showLayersWindow, state.sceneOpen);
            ImGui::MenuItem("Tools", nullptr, &state.showToolsWindow);
            ImGui::MenuItem("History", nullptr, &state.showUndoWindow);
            ImGui::Separator();
            if (ImGui::MenuItem("Toggle Panels", "Ctrl+E")) {
                bool target = !(state.showSceneWindow || state.showPropertiesWindow || state.showLayersWindow || state.showToolsWindow);
                state.showSceneWindow = target;
                state.showPropertiesWindow = target;
                state.showLayersWindow = target;
                state.showToolsWindow = target;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Debug")) {
            ImGui::MenuItem("Show Grid", nullptr, &state.showGrid);
            ImGui::MenuItem("Show Origin", nullptr, &state.showOrigin);
            ImGui::MenuItem("Show Gizmos", nullptr, &state.showGizmo);
            ImGui::Separator();
            ImGui::MenuItem("Show AABBs", nullptr, &state.showAABBs);
            ImGui::MenuItem("Show Selected AABB", nullptr, &state.showSelectedAABB);
            ImGui::MenuItem("Show Light Gizmos", nullptr, &state.showLightGizmos);
            ImGui::Separator();
            ImGui::MenuItem("Show Buffer Overlay", nullptr, &state.showBufferOverlay);
            ImGui::MenuItem("Show Shadow Overlay", nullptr, &state.showShadowOverlay);
            ImGui::Separator();
            ImGui::MenuItem("Frame Graph", nullptr, &state.showFrameGraph);
            ImGui::Separator();
            ImGui::Text("Fullscreen Buffer View");
            ImGui::RadioButton("Albedo", &state.gbufferView, 1);
            ImGui::RadioButton("Normals", &state.gbufferView, 2);
            ImGui::RadioButton("Depth", &state.gbufferView, 3);
            ImGui::RadioButton("Shadow Factor", &state.gbufferView, 4);
            ImGui::RadioButton("Shadow Map", &state.gbufferView, 5);
            ImGui::RadioButton("Shadow UV", &state.gbufferView, 6);
            ImGui::RadioButton("World Pos", &state.gbufferView, 7);
            ImGui::RadioButton("Lit", &state.gbufferView, 0);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}
