#pragma once

#include "debugdraw.h"
#include "gizmo.h"
#include "lightingpass.h"
#include "rhieditorui.h"

#include <glm/glm.hpp>

#include <vector>

struct RenderSnapshot {
    glm::mat4 viewMatrix;
    glm::mat4 projMatrix;

    int windowWidth = 0;
    int windowHeight = 0;

    float mouseX = 0;
    float mouseY = 0;

    bool showGizmo = true;
    GBufferView gbufferViewMode = GBufferView::Lit;
    bool showBufferOverlay = false;

    std::vector<GizmoVertex> translateGizmoVerts;
    std::vector<GizmoVertex> rotateGizmoVerts;
    std::vector<GizmoVertex> scaleGizmoVerts;

    DebugDrawData debugData;
    ImGuiFrameSnapshot imguiSnapshot;
};
