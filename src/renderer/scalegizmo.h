#pragma once

#include "gizmo.h"
#include "scenetypes.h"

#include <optional>
#include <span>
#include <vector>

class ScaleGizmo {
public:
    auto update(RhiExtent2D extent, const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPos, float mouseX, float mouseY, bool visible,
        const glm::vec3& originWorld) -> void;

    auto vertices() const -> std::span<const GizmoVertex> { return frameVertices; }

    auto tryGrab(float mouseX, float mouseY, RhiExtent2D extent, const glm::mat4& view, const glm::mat4& proj, const glm::vec3& gizmoAnchor,
        const Transform& currentLocal, const glm::mat4& currentWorld) -> bool;

    auto dragUpdate(float mouseX, float mouseY, RhiExtent2D extent, const glm::mat4& view, const glm::mat4& proj) const -> std::optional<Transform>;

    auto release() -> void { dragAxis = -1; }
    auto isDragging() const -> bool { return dragAxis >= 0; }
    auto dragStartLocalTransform() const -> const Transform& { return dragStartLocal; }

private:
    auto findClosestAxis(float mouseX, float mouseY) const -> int;
    auto axisParam(float mouseX, float mouseY, RhiExtent2D extent, const glm::mat4& view, const glm::mat4& proj, int axis) const -> float;

    glm::mat4 viewProj{1.0f};
    glm::vec3 origin{0.0f};
    float scale = 1.0f;
    RhiExtent2D extent{};
    bool visible = false;
    int hoveredAxis = -1;

    int dragAxis = -1;
    int dragLocalAxis = -1; // which LOCAL scale component the world drag axis maps to
    float dragStartT = 0.0f;
    Transform dragStartLocal{};
    glm::vec3 dragStartAnchor{0.0f};
    glm::vec3 dragStartAnchorLocal{0.0f}; // anchor in the prim's local (pre-scale) space

    std::vector<GizmoVertex> frameVertices;
};
