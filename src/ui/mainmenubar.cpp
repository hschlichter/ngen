#include "mainmenubar.h"

#include "sceneupdater.h"
#include "undostack.h"

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
            ImGui::Separator();
            ImGui::MenuItem("Show Buffer Overlay", nullptr, &state.showBufferOverlay);
            ImGui::Separator();
            ImGui::Text("Fullscreen Buffer View");
            ImGui::RadioButton("Albedo", &state.gbufferView, 1);
            ImGui::RadioButton("Normals", &state.gbufferView, 2);
            ImGui::RadioButton("Depth", &state.gbufferView, 3);
            ImGui::RadioButton("Lit", &state.gbufferView, 0);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}
