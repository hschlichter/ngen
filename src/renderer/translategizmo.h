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

    auto release() -> void { dragHandle = -1; }
    auto isDragging() const -> bool { return dragHandle >= 0; }

    auto dragStartLocalTransform() const -> const Transform& { return dragStartLocal; }

private:
    // Returns 0-2 for single-axis (X/Y/Z), 3-5 for plane handles
    // (3=YZ normal-X, 4=XZ normal-Y, 5=XY normal-Z), -1 for miss.
    auto findClosestHandle(float mouseX, float mouseY) const -> int;
    auto axisParam(float mouseX, float mouseY, RhiExtent2D extent, const glm::mat4& view, const glm::mat4& proj, int axis) const -> float;
    auto planeHit(float mouseX, float mouseY, RhiExtent2D extent, const glm::mat4& view, const glm::mat4& proj, int normalAxis) const -> glm::vec3;

    glm::mat4 viewProj{1.0f};
    glm::vec3 origin{0.0f};
    float scale = 1.0f;
    RhiExtent2D extent{};
    bool visible = false;
    int hoveredHandle = -1; // 0-2 axes, 3-5 planes

    // Drag state — captured at tryGrab().
    int dragHandle = -1;               // 0-2 single-axis, 3-5 plane
    float dragStartT = 0.0f;           // for axis drag (0-2)
    glm::vec3 dragStartPlaneHit{0.0f}; // for plane drag (3-5)
    Transform dragStartLocal{};
    glm::vec3 dragStartAnchor{0.0f};
    glm::vec3 dragStartPrimWorld{0.0f};
    glm::mat4 dragStartParentInv{1.0f};

    std::vector<GizmoVertex> frameVertices;
};
