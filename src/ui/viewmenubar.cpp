#include "viewmenubar.h"

#include "debugview.h"

#include <imgui.h>

auto drawViewMenuBar(ViewMenuBarState& state) -> void {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Quit")) {
            state.requestQuit = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Deselect", "Z", false, state.hasSelection)) {
            state.requestDeselect = true;
        }
        if (ImGui::MenuItem("Select Parent", "R", false, state.hasSelection)) {
            state.requestSelectParent = true;
        }
        if (ImGui::MenuItem("Frame Selected", "F", false, state.hasSelection)) {
            state.requestFrameSelected = true;
        }
        ImGui::Separator();
        ImGui::RadioButton("Select (1)", &state.tool, 0);
        ImGui::RadioButton("Translate (2)", &state.tool, 1);
        ImGui::RadioButton("Rotate (3)", &state.tool, 2);
        ImGui::RadioButton("Scale (4)", &state.tool, 3);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Show Guides", nullptr, &state.showGuides);
        ImGui::MenuItem("Show Proxies", nullptr, &state.showProxies);
        ImGui::Separator();
        ImGui::MenuItem("Show Grid", nullptr, &state.showGrid);
        ImGui::MenuItem("Show Origin", nullptr, &state.showOrigin);
        ImGui::MenuItem("Show Gizmos", nullptr, &state.showGizmo);
        ImGui::MenuItem("Show AABBs", nullptr, &state.showAABBs);
        ImGui::MenuItem("Show Selected AABB", nullptr, &state.showSelectedAABB);
        ImGui::MenuItem("Show Light Gizmos", nullptr, &state.showLightGizmos);
        ImGui::Separator();
        ImGui::MenuItem("Depth Prepass", nullptr, &state.depthPrepass);
        ImGui::MenuItem("Show Buffer Overlay", nullptr, &state.showBufferOverlay);
        ImGui::MenuItem("Show Shadow Overlay", nullptr, &state.showShadowOverlay);
        ImGui::MenuItem("Anti-Aliasing (FXAA)", nullptr, &state.antiAliasing);
        ImGui::Separator();
        ImGui::Text("Fullscreen Buffer View");
        ImGui::RadioButton("Albedo", &state.gbufferView, 1);
        ImGui::RadioButton("Normals", &state.gbufferView, 2);
        ImGui::RadioButton("Depth", &state.gbufferView, 3);
        ImGui::RadioButton("Shadow Factor", &state.gbufferView, 4);
        ImGui::RadioButton("Shadow Map", &state.gbufferView, 5);
        ImGui::RadioButton("Shadow UV", &state.gbufferView, 6);
        ImGui::RadioButton("World Pos", &state.gbufferView, 7);
        ImGui::RadioButton("Mip Level", &state.gbufferView, 8);
        ImGui::RadioButton("Cascades", &state.gbufferView, 9);
        ImGui::RadioButton("LOD Level", &state.gbufferView, 10);
        ImGui::RadioButton("Lit", &state.gbufferView, 0);
        ImGui::Separator();
        ImGui::Text("Debug View");
        for (int i = 0; i < (int) debugViewCount; i++) {
            ImGui::RadioButton(debugViewLabels[i], &state.debugView, i);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Windows")) {
        ImGui::MenuItem("Frame Graph", nullptr, &state.showFrameGraph);
        ImGui::MenuItem("Performance", nullptr, &state.showPerformance);
        ImGui::MenuItem("Status Bar", nullptr, &state.showStatusBar);
        ImGui::MenuItem("Render Debug", nullptr, &state.showRenderDebug);
        ImGui::MenuItem("Camera", nullptr, &state.showCamera);
        ImGui::MenuItem("Culling", nullptr, &state.showCulling);
        ImGui::MenuItem("Assets", nullptr, &state.showAssets);
        ImGui::MenuItem("LOD", nullptr, &state.showLod);
        if (ImGui::BeginMenu("Introspection")) {
            ImGui::MenuItem("Capture", nullptr, &state.introspection.capture);
            ImGui::MenuItem("Frame Debugger", nullptr, &state.introspection.frameDebugger);
            ImGui::MenuItem("GPU Scene", nullptr, &state.introspection.gpuScene);
            ImGui::MenuItem("Counters", nullptr, &state.introspection.counters);
            ImGui::MenuItem("Memory", nullptr, &state.introspection.memory);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Capture Frame (RenderDoc)", nullptr, false, state.introspection.renderDocAvailable)) {
            state.introspection.renderDocCaptureRequested = true;
        }
        if (ImGui::MenuItem("Open Last Capture", nullptr, false, state.introspection.renderDocAvailable)) {
            state.introspection.renderDocOpenRequested = true;
        }
        if (!state.introspection.renderDocAvailable) {
            ImGui::TextDisabled("RenderDoc not loaded (start with --renderdoc)");
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}
