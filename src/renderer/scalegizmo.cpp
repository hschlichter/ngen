#include "scalegizmo.h"

#include <cmath>

static constexpr glm::vec3 kAxisDirs[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static constexpr glm::vec4 kAxisColors[3] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0.4f, 1, 1}};
static constexpr glm::vec4 kHotColor = {1, 1, 0, 1};
static constexpr float kPixelLength = 90.0f;
static constexpr float kHitThresholdPx = 10.0f;

static auto mouseRay(float mx, float my, RhiExtent2D ext, const glm::mat4& view, const glm::mat4& proj) -> std::pair<glm::vec3, glm::vec3> {
    float ndcX = (2.0f * mx / (float) ext.width) - 1.0f;
    float ndcY = (2.0f * my / (float) ext.height) - 1.0f;
    auto invVP = glm::inverse(proj * view);
    auto n = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    auto f = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    n /= n.w;
    f /= f.w;
    return {glm::vec3(n), glm::normalize(glm::vec3(f - n))};
}

auto ScaleGizmo::update(RhiExtent2D ext,
                        const glm::mat4& view,
                        const glm::mat4& proj,
                        const glm::vec3& cameraPos,
                        float mouseX,
                        float mouseY,
                        bool vis,
                        const glm::vec3& originWorld) -> void {
    this->extent = ext;
    this->viewProj = proj * view;
    this->visible = vis;
    this->origin = isDragging() ? dragStartAnchor : originWorld;

    float dist = glm::length(cameraPos - this->origin);
    this->scale = std::max(1e-4f, kPixelLength * (dist * 2.0f) / (std::abs(proj[1][1]) * (float) ext.height));

    hoveredAxis = (vis && !isDragging()) ? findClosestAxis(mouseX, mouseY) : hoveredAxis;
    if (!vis) {
        hoveredAxis = -1;
    }

    frameVertices.clear();
    if (!vis) {
        return;
    }

    for (int i = 0; i < 3; i++) {
        auto dir = kAxisDirs[i];
        bool hot = (i == hoveredAxis) || (i == dragAxis);
        auto color = hot ? kHotColor : kAxisColors[i];
        auto tip = this->origin + dir * this->scale;

        // Shaft
        frameVertices.push_back({this->origin, color});
        frameVertices.push_back({tip, color});

        // Small box at the tip: 4 line segments forming a diamond in the plane
        // perpendicular to the axis, to visually distinguish from translate arrows.
        // auto perpA = kAxisDirs[(i + 1) % 3];
        // auto perpB = kAxisDirs[(i + 2) % 3];
        // float boxSize = this->scale * 0.08f;
        // auto pa = tip + perpA * boxSize;
        // auto pb = tip + perpB * boxSize;
        // auto na = tip - perpA * boxSize;
        // auto nb = tip - perpB * boxSize;
        // frameVertices.push_back({pa, color});
        // frameVertices.push_back({pb, color});
        // frameVertices.push_back({pb, color});
        // frameVertices.push_back({na, color});
        // frameVertices.push_back({na, color});
        // frameVertices.push_back({nb, color});
        // frameVertices.push_back({nb, color});
        // frameVertices.push_back({pa, color});
    }
}

auto ScaleGizmo::findClosestAxis(float mouseX, float mouseY) const -> int {
    auto toScreen = [&](glm::vec3 p) -> glm::vec2 {
        auto clip = viewProj * glm::vec4(p, 1.0f);
        if (clip.w <= 0.0f) {
            return {-1e9f, -1e9f};
        }
        auto ndc = glm::vec3(clip) / clip.w;
        return {(ndc.x * 0.5f + 0.5f) * (float) extent.width, (ndc.y * 0.5f + 0.5f) * (float) extent.height};
    };
    auto distToSeg = [](glm::vec2 p, glm::vec2 a, glm::vec2 b) {
        auto ab = b - a;
        float denom = glm::dot(ab, ab);
        if (denom <= 1e-6f) {
            return glm::length(p - a);
        }
        float t = glm::clamp(glm::dot(p - a, ab) / denom, 0.0f, 1.0f);
        return glm::length(p - (a + t * ab));
    };

    auto mouse = glm::vec2(mouseX, mouseY);
    auto originScreen = toScreen(origin);
    float bestDist = kHitThresholdPx;
    int best = -1;
    for (int i = 0; i < 3; i++) {
        float d = distToSeg(mouse, originScreen, toScreen(origin + kAxisDirs[i] * scale));
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

auto ScaleGizmo::axisParam(float mouseX, float mouseY, RhiExtent2D ext, const glm::mat4& view, const glm::mat4& proj, int axis) const -> float {
    auto [ro, rd] = mouseRay(mouseX, mouseY, ext, view, proj);
    auto ad = kAxisDirs[axis];
    auto w0 = dragStartAnchor - ro;
    float b = glm::dot(ad, rd);
    float denom = 1.0f - b * b;
    if (std::abs(denom) < 1e-6f) {
        return dragStartT;
    }
    return (b * glm::dot(rd, w0) - glm::dot(ad, w0)) / denom;
}

auto ScaleGizmo::tryGrab(float mouseX,
                         float mouseY,
                         RhiExtent2D ext,
                         const glm::mat4& view,
                         const glm::mat4& proj,
                         const glm::vec3& gizmoAnchor,
                         const Transform& currentLocal,
                         const glm::mat4& currentWorld) -> bool {
    if (!visible) {
        return false;
    }
    int axis = findClosestAxis(mouseX, mouseY);
    if (axis < 0) {
        return false;
    }
    dragAxis = axis;
    dragStartLocal = currentLocal;
    dragStartAnchor = gizmoAnchor;
    dragStartT = axisParam(mouseX, mouseY, ext, view, proj, axis);

    // Determine which LOCAL axis the world-space drag direction maps to.
    // The gizmo handles are world-aligned, but scale components are in local
    // space — these differ when the prim (or its parent chain) has rotation
    // (e.g. the Z-up→Y-up stage correction).
    {
        auto worldDir = kAxisDirs[axis];
        auto m = glm::mat3(currentWorld);
        float bestDot = 0.0f;
        dragLocalAxis = 0;
        for (int j = 0; j < 3; j++) {
            float d = std::abs(glm::dot(glm::normalize(m[j]), worldDir));
            if (d > bestDot) {
                bestDot = d;
                dragLocalAxis = j;
            }
        }
    }

    // Compute the anchor in the prim's local (pre-scale) coordinate space so
    // dragUpdate can adjust position to keep the anchor stationary after scale.
    // parent_space_point = pos + R * (scale * local_point)
    //   → local_point = invR * (parent_space_point - pos) / scale
    auto parentWorld = currentWorld * glm::inverse(currentLocal.toMat4());
    auto anchorInParent = glm::vec3(glm::inverse(parentWorld) * glm::vec4(gizmoAnchor, 1.0f));
    auto invR = glm::inverse(glm::mat3_cast(currentLocal.rotation));
    dragStartAnchorLocal = invR * (anchorInParent - currentLocal.position) / currentLocal.scale;

    return true;
}

auto ScaleGizmo::dragUpdate(float mouseX, float mouseY, RhiExtent2D ext, const glm::mat4& view, const glm::mat4& proj) const -> std::optional<Transform> {
    if (!isDragging()) {
        return std::nullopt;
    }
    float t = axisParam(mouseX, mouseY, ext, view, proj, dragAxis);
    // Scale factor = ratio of current distance to initial distance along the axis.
    // Avoid division by zero; clamp to a minimum positive scale.
    float factor = (std::abs(dragStartT) > 1e-4f) ? t / dragStartT : 1.0f;
    factor = std::max(factor, 0.01f);

    auto newLocal = dragStartLocal;
    newLocal.scale[dragLocalAxis] = dragStartLocal.scale[dragLocalAxis] * factor;

    // Compensate position so the gizmo anchor stays fixed in world space.
    // In parent space: anchor_parent = pos + R * (scale * anchorLocal).
    // For this to remain constant: pos_new = pos_old + R * ((oldScale - newScale) * anchorLocal).
    auto R = glm::mat3_cast(dragStartLocal.rotation);
    auto scaleDiff = dragStartLocal.scale - newLocal.scale;
    newLocal.position = dragStartLocal.position + R * (scaleDiff * dragStartAnchorLocal);

    return newLocal;
}
