#pragma once

#include "gizmo.h"
#include "scenetypes.h"

#include <optional>
#include <span>
#include <vector>

// World-space, world-axis-aligned translate gizmo. Lives entirely on the main
// thread: callers feed it the current camera + mouse and get back vertices for
// the renderer to draw plus, during a drag, the new local Transform to apply.
class TranslateGizmo {
public:
    // Per-frame: rebuild arrow vertices and snapshot screen layout for picking.
    // Pass `visible = false` (or no selection) to clear vertices.
    auto update(RhiExtent2D extent,
                const glm::mat4& view,
                const glm::mat4& proj,
                const glm::vec3& cameraPos,
                float mouseX,
                float mouseY,
                bool visible,
                const glm::vec3& originWorld) -> void;

    auto vertices() const -> std::span<const GizmoVertex> { return frameVertices; }

    // Try to grab a handle at the cursor. Captures the prim's transform so
    // subsequent dragUpdate() calls produce a complete new local Transform.
    // `gizmoAnchor` is where the gizmo handles are visually drawn (e.g. the
    // selected prim's bounds center); the axis line passes through it.
    // `currentWorld` is the prim's *actual* world transform — its translation
    // column is what dragUpdate will offset to compute the new local position.
    auto tryGrab(float mouseX,
                 float mouseY,
                 RhiExtent2D extent,
                 const glm::mat4& view,
                 const glm::mat4& proj,
                 const glm::vec3& gizmoAnchor,
                 const Transform& currentLocal,
                 const glm::mat4& currentWorld) -> bool;

    // Returns the new local Transform under the cursor, or nullopt if not dragging.
    auto dragUpdate(float mouseX, float mouseY, RhiExtent2D extent, const glm::mat4& view, const glm::mat4& proj) const -> std::optional<Transform>;

    auto release() -> void { dragAxis = -1; }
    auto isDragging() const -> bool { return dragAxis >= 0; }

private:
    auto findClosestAxis(float mouseX, float mouseY) const -> int;
    auto axisParam(float mouseX, float mouseY, RhiExtent2D extent, const glm::mat4& view, const glm::mat4& proj, int axis) const -> float;

    // Layout from the most recent update() — used for hit testing.
    glm::mat4 viewProj{1.0f};
    glm::vec3 origin{0.0f};
    float scale = 1.0f;
    RhiExtent2D extent{};
    bool visible = false;
    int hoveredAxis = -1;

    // Drag state — captured at tryGrab().
    int dragAxis = -1;
    float dragStartT = 0.0f;
    Transform dragStartLocal{};
    glm::vec3 dragStartAnchor{0.0f};   // visual anchor / axis line passes through this
    glm::vec3 dragStartPrimWorld{0.0f}; // prim's world translation; new world = this + delta
    glm::mat4 dragStartParentInv{1.0f};

    std::vector<GizmoVertex> frameVertices;
};
