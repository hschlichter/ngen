#pragma once

#include "editoruipass.h"
#include "imguibackend.h"
#include "renderextension.h"

class FrameGraph;

// ImGui's draw data for one frame, made on the main thread (UiHooks::fillSnapshot) and deep-copied, so the render thread draws it while the
// main thread builds the next.
struct UiFrame : RenderExtensionData {
    ImGuiFrameSnapshot imgui;
};

// The UI's part of the renderer's frame (a render extension): the backend set up with the renderer, and ImGui drawn last on the backbuffer,
// except on a screenshot frame taken without the UI.
class UiRender : public RenderExtension {
public:
    explicit UiRender(ImGuiBackend& backend) : backend(backend) {}

    auto init(const RenderExtensionContext& context) -> bool override;
    auto addOverlayPasses(Renderer& renderer, RenderSnapshot& snapshot, const RenderFrame& frame) -> void override;
    auto destroy() -> void override;

private:
    ImGuiBackend& backend;
    FrameGraph* frameGraph = nullptr;
    EditorUIPass pass;
};
