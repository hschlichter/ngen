#pragma once

#include "gizmo.h"

#include <vector>

#include <optional>

// The world's axes in a corner of the view, turned as the camera rendered from; a click on one picks it, for the application to turn its
// camera to look along it.
class Axis3DGizmo {
public:
    auto draw(RhiExtent2D fullExtent, const glm::mat4& viewMatrix) -> GizmoDrawRequest;
    auto updateHover(float mouseX, float mouseY) -> void;
    // The axis (0 x, 1 y, 2 z) under the cursor, as last drawn; none off the gizmo.
    auto hitTest(float mouseX, float mouseY) const -> std::optional<int>;

    // Where it sits: a square of gizmoSize pixels, margin pixels in from the view's top-right corner. Viewport overlays keep clear of it.
    static constexpr uint32_t gizmoSize = 120;
    static constexpr uint32_t margin = 50;

private:
    auto findClosestAxis(float mouseX, float mouseY) const -> int;

    glm::mat4 lastViewProj = glm::mat4(1.0f);
    glm::mat4 lastViewMatrix = glm::mat4(1.0f);
    RhiExtent2D lastFullExtent = {};
    int hoveredAxis = -1;

    std::vector<GizmoVertex> frameVertices;
};
