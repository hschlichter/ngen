#pragma once

#include "imguibackend.h"
#include "statusbar.h"
#include "viewhooks.h"

#include <cstdint>

class ImGuiBackendVulkan;
class RenderDocCapture;
class RhiDevice;
class ToolsHooks;
class ViewUI;

// The UI's part of ngen-view's frame: ImGui's input and frame, the windows drawn and what they asked for carried out (selection, framing,
// go-to and repacks, texture inspection, screenshots, RenderDoc), their readbacks, and the lists and numbers they show. It wraps the tools'
// hooks, which run inside it: the tools' windows draw the tools' state, and the tools carry out what those windows asked for.
class UiHooks : public ViewHooks {
public:
    UiHooks(ViewUI& ui, ToolsHooks& tools, ImGuiBackendVulkan& imgui, const RhiDevice& device, RenderDocCapture& renderDoc)
        : ui(ui), tools(tools), imgui(imgui), device(device), renderDoc(renderDoc) {}

    auto afterScene(ViewFrame& frame) -> void override;
    auto cameras(ViewFrame& frame) -> void override;
    auto event(ViewFrame& frame, const SDL_Event& event) -> bool override;
    auto afterCamera(ViewFrame& frame) -> void override;
    auto debugDraw(ViewFrame& frame) -> void override;
    auto wantReadbacks(ViewFrame& frame) -> void override;
    auto takeReadbacks(ViewFrame& frame) -> void override;
    auto drawUi(ViewFrame& frame) -> void override;
    auto fillSnapshot(ViewFrame& frame, RenderSnapshot& snapshot) -> void override;

private:
    ViewUI& ui;
    ToolsHooks& tools;
    ImGuiBackendVulkan& imgui;
    const RhiDevice& device;
    RenderDocCapture& renderDoc;
    StatusBarGatherer statusBar;
    ImGuiFrameSnapshot imguiSnapshot;         // this frame's draw data, from drawUi to fillSnapshot
    uint64_t cameraListSnapshot = UINT64_MAX; // the scene snapshot the Camera window's list was built from
};
