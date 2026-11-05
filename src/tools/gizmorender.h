#pragma once

#include "gizmo.h"
#include "gizmopass.h"
#include "renderextension.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// The gizmos' lines for one frame, made on the main thread (ToolsHooks::fillSnapshot): the axis gizmo in its corner, the transform gizmo on
// the selection, each with its own view-projection and viewport.
struct GizmoFrame : RenderExtensionData {
    struct Draw {
        std::vector<GizmoVertex> vertices;
        glm::mat4 viewProj = glm::mat4(1.0f);
        int32_t vpX = 0;
        int32_t vpY = 0;
        RhiExtent2D vpExtent = {}; // none: the whole backbuffer
    };
    std::vector<Draw> draws;
};

// The tools' part of the renderer's frame (a render extension): the gizmos drawn on the backbuffer after the debug lines.
class GizmoRender : public RenderExtension {
public:
    auto init(const RenderExtensionContext& context) -> bool override;
    auto addOverlayPasses(Renderer& renderer, RenderSnapshot& snapshot, const RenderFrame& frame) -> void override;
    auto destroy() -> void override;

private:
    RhiDevice* device = nullptr;
    FrameGraph* frameGraph = nullptr;
    GizmoPass pass;
    std::vector<GizmoDrawRequest> requests;
};
