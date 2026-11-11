#include "viewmenubar.h"

#include "configwindow.h"
#include "debugview.h"

#include <imgui.h>

#include <format>
#include <string>

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
        ImGui::RadioButton("Select", &state.tool, 0);
        ImGui::RadioButton("Translate", &state.tool, 1);
        ImGui::RadioButton("Rotate", &state.tool, 2);
        ImGui::RadioButton("Scale", &state.tool, 3);
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
        configMenuItem("Frame Budget", "Ctrl+B", "ui.overlay.budget");
        ImGui::Separator();
        ImGui::MenuItem("Depth Prepass", nullptr, &state.depthPrepass);
        ImGui::MenuItem("Show Buffer Overlay", "Ctrl+1", &state.showBufferOverlay);
        ImGui::MenuItem("Show Shadow Overlay", "Ctrl+2", &state.showShadowOverlay);
        ImGui::MenuItem("Anti-Aliasing (FXAA)", nullptr, &state.antiAliasing);
        ImGui::Separator();
        configMenuItem("Follow Ground", "G", "view.camera.follow");
        bool debugCamera = configMenuItem("Debug Camera", "Ctrl+D", "tools.debugcamera.on");
        configMenuItem("Controls Move Main Camera", "C", "tools.debugcamera.controlMain", debugCamera);
        ImGui::Separator();
        ImGui::Text("Fullscreen Buffer View");
        ImGui::RadioButton("Albedo", &state.gbufferView, 1);
        ImGui::RadioButton("Normals", &state.gbufferView, 2);
        ImGui::RadioButton("Depth", &state.gbufferView, 3);
        ImGui::RadioButton("Shadow Factor", &state.gbufferView, 4);
        ImGui::RadioButton("Shadow Map", &state.gbufferView, 5);
        ImGui::RadioButton("Shadow UV", &state.gbufferView, 6);
        ImGui::RadioButton("World Pos", &state.gbufferView, 7);
        ImGui::RadioButton("Mip Level (Ctrl+3)", &state.gbufferView, 8);
        ImGui::RadioButton("Cascades", &state.gbufferView, 9);
        ImGui::RadioButton("LOD Level (Ctrl+4)", &state.gbufferView, 10);
        ImGui::RadioButton("Depth Pyramid (occlusion)", &state.gbufferView, 11);
        ImGui::RadioButton("Lit (Ctrl+5: everything off, again to restore)", &state.gbufferView, 0);
        ImGui::Separator();
        ImGui::Text("Debug View");
        // The views with a key (ViewUI::toggleShortcuts) show it.
        for (int i = 0; i < (int) debugViewCount; i++) {
            const char* key = nullptr;
            switch ((DebugView) i) {
                case DebugView::Wireframe:
                    key = "1";
                    break;
                case DebugView::TriangleSize:
                    key = "2";
                    break;
                case DebugView::Overdraw:
                    key = "3";
                    break;
                case DebugView::InstanceId:
                    key = "4";
                    break;
                case DebugView::Streaming:
                    key = "5";
                    break;
                default:
                    break;
            }
            auto label = key != nullptr ? std::format("{} ({})", debugViewLabels[i], key) : std::string(debugViewLabels[i]);
            ImGui::RadioButton(label.c_str(), &state.debugView, i);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Windows")) {
        ImGui::Separator();
        ImGui::MenuItem("Frame Graph", nullptr, &state.showFrameGraph);
        ImGui::MenuItem("Performance", "Ctrl+P", &state.showPerformance);
        ImGui::MenuItem("Configuration", nullptr, &state.showConfig);
        ImGui::MenuItem("Status Bar", nullptr, &state.showStatusBar);
        ImGui::MenuItem("Render Debug", nullptr, &state.showRenderDebug);
        ImGui::MenuItem("Camera", "Ctrl+C", &state.showCamera);
        ImGui::MenuItem("Culling", nullptr, &state.showCulling);
        ImGui::MenuItem("Assets", "Ctrl+A", &state.showAssets);
        ImGui::MenuItem("LOD", "Ctrl+L", &state.showLod);
        ImGui::MenuItem("Streaming", "Ctrl+S", &state.showStreaming);
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
