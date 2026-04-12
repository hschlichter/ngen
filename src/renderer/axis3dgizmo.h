#pragma once

#include "gizmo.h"

#include <vector>

struct Camera;

class Axis3DGizmo {
public:
    Axis3DGizmo() = default;
    Axis3DGizmo(Camera* camera);

    auto draw(RhiExtent2D fullExtent, const glm::mat4& viewMatrix) -> GizmoDrawRequest;
    auto updateHover(float mouseX, float mouseY) -> void;
    auto hitTest(float mouseX, float mouseY, RhiExtent2D windowExtent) -> bool;

private:
    auto findClosestAxis(float mouseX, float mouseY) const -> int;

    Camera* camera = nullptr;
    glm::mat4 lastViewProj = glm::mat4(1.0f);
    glm::mat4 lastViewMatrix = glm::mat4(1.0f);
    RhiExtent2D lastFullExtent = {};
    int hoveredAxis = -1;

    static constexpr uint32_t gizmoSize = 120;
    static constexpr uint32_t margin = 50;

    std::vector<GizmoVertex> frameVertices;
};
