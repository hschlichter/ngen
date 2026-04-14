#include "translategizmo.h"

#include <cmath>

static constexpr glm::vec3 kAxisDirs[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static constexpr glm::vec4 kAxisColors[3] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0.4f, 1, 1}};
static constexpr glm::vec4 kHotColor = {1, 1, 0, 1};
// Plane handle colors: use the normal axis's color at reduced brightness.
static constexpr glm::vec4 kPlaneColors[3] = {{0.6f, 0, 0, 1}, {0, 0.6f, 0, 1}, {0, 0.2f, 0.6f, 1}};
static constexpr float kPixelLength = 90.0f;
static constexpr float kHitThresholdPx = 10.0f;
// Plane handle quad: fraction of gizmo scale for the inner/outer edges.
static constexpr float kPlaneHandleLo = 0.0f;
static constexpr float kPlaneHandleHi = 0.25f;
static constexpr int kPlaneFillLines = 8; // parallel lines to approximate a solid fill

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

auto TranslateGizmo::update(RhiExtent2D ext,
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

    hoveredHandle = (vis && !isDragging()) ? findClosestHandle(mouseX, mouseY) : hoveredHandle;
    if (!vis) {
        hoveredHandle = -1;
    }

    frameVertices.clear();
    if (!vis) {
        return;
    }

    // Single-axis arrows.
    for (int i = 0; i < 3; i++) {
        auto color = (i == hoveredHandle || i == dragHandle) ? kHotColor : kAxisColors[i];
        frameVertices.push_back({this->origin, color});
        frameVertices.push_back({this->origin + kAxisDirs[i] * this->scale, color});
    }

    // Plane handles: small filled quads between pairs of axes, close to the origin.
    // Filled by drawing parallel lines with the pipeline's lineWidth=4.
    for (int n = 0; n < 3; n++) {
        int a = (n + 1) % 3;
        int b = (n + 2) % 3;
        bool hot = ((3 + n) == hoveredHandle) || ((3 + n) == dragHandle);
        auto color = hot ? kHotColor : kPlaneColors[n];
        float lo = kPlaneHandleLo * this->scale;
        float hi = kPlaneHandleHi * this->scale;
        // Outline.
        auto p0 = this->origin + kAxisDirs[a] * lo + kAxisDirs[b] * lo;
        auto p1 = this->origin + kAxisDirs[a] * hi + kAxisDirs[b] * lo;
        auto p2 = this->origin + kAxisDirs[a] * hi + kAxisDirs[b] * hi;
        auto p3 = this->origin + kAxisDirs[a] * lo + kAxisDirs[b] * hi;
        frameVertices.push_back({p0, color});
        frameVertices.push_back({p1, color});
        frameVertices.push_back({p1, color});
        frameVertices.push_back({p2, color});
        frameVertices.push_back({p2, color});
        frameVertices.push_back({p3, color});
        frameVertices.push_back({p3, color});
        frameVertices.push_back({p0, color});
        // Fill: parallel lines along axis `a`, spaced evenly along axis `b`.
        for (int f = 0; f < kPlaneFillLines; f++) {
            float t = lo + (hi - lo) * ((float) f + 0.5f) / (float) kPlaneFillLines;
            auto lineStart = this->origin + kAxisDirs[a] * lo + kAxisDirs[b] * t;
            auto lineEnd = this->origin + kAxisDirs[a] * hi + kAxisDirs[b] * t;
            frameVertices.push_back({lineStart, color});
            frameVertices.push_back({lineEnd, color});
        }
    }
}

auto TranslateGizmo::findClosestHandle(float mouseX, float mouseY) const -> int {
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
    // Point-in-quad test via cross-product winding.
    auto pointInQuad = [](glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d) {
        auto cross2d = [](glm::vec2 e, glm::vec2 f) {
            return e.x * f.y - e.y * f.x;
        };
        float d0 = cross2d(b - a, p - a);
        float d1 = cross2d(c - b, p - b);
        float d2 = cross2d(d - c, p - c);
        float d3 = cross2d(a - d, p - d);
        bool allPos = (d0 >= 0) && (d1 >= 0) && (d2 >= 0) && (d3 >= 0);
        bool allNeg = (d0 <= 0) && (d1 <= 0) && (d2 <= 0) && (d3 <= 0);
        return allPos || allNeg;
    };

    auto mouse = glm::vec2(mouseX, mouseY);

    // Check plane handles first (take priority when cursor is inside the quad).
    for (int n = 0; n < 3; n++) {
        int a = (n + 1) % 3;
        int b = (n + 2) % 3;
        float lo = kPlaneHandleLo * scale;
        float hi = kPlaneHandleHi * scale;
        auto p0 = toScreen(origin + kAxisDirs[a] * lo + kAxisDirs[b] * lo);
        auto p1 = toScreen(origin + kAxisDirs[a] * hi + kAxisDirs[b] * lo);
        auto p2 = toScreen(origin + kAxisDirs[a] * hi + kAxisDirs[b] * hi);
        auto p3 = toScreen(origin + kAxisDirs[a] * lo + kAxisDirs[b] * hi);
        if (pointInQuad(mouse, p0, p1, p2, p3)) {
            return 3 + n;
        }
    }

    // Then check single-axis handles.
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

auto TranslateGizmo::axisParam(float mouseX, float mouseY, RhiExtent2D ext, const glm::mat4& view, const glm::mat4& proj, int axis) const -> float {
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

auto TranslateGizmo::planeHit(float mouseX, float mouseY, RhiExtent2D ext, const glm::mat4& view, const glm::mat4& proj, int normalAxis) const -> glm::vec3 {
    auto [ro, rd] = mouseRay(mouseX, mouseY, ext, view, proj);
    auto normal = kAxisDirs[normalAxis];
    float denom = glm::dot(rd, normal);
    if (std::abs(denom) < 1e-6f) {
        return dragStartPlaneHit; // ray parallel to plane
    }
    float t = glm::dot(dragStartAnchor - ro, normal) / denom;
    return ro + rd * t;
}

auto TranslateGizmo::tryGrab(float mouseX,
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
    int handle = findClosestHandle(mouseX, mouseY);
    if (handle < 0) {
        return false;
    }
    dragHandle = handle;
    dragStartLocal = currentLocal;
    dragStartAnchor = gizmoAnchor;
    dragStartPrimWorld = glm::vec3(currentWorld[3]);
    dragStartParentInv = glm::inverse(currentWorld * glm::inverse(currentLocal.toMat4()));

    if (handle < 3) {
        dragStartT = axisParam(mouseX, mouseY, ext, view, proj, handle);
    } else {
        dragStartPlaneHit = planeHit(mouseX, mouseY, ext, view, proj, handle - 3);
    }
    return true;
}

auto TranslateGizmo::dragUpdate(float mouseX, float mouseY, RhiExtent2D ext, const glm::mat4& view, const glm::mat4& proj) const -> std::optional<Transform> {
    if (!isDragging()) {
        return std::nullopt;
    }

    glm::vec3 desiredPrimWorld;
    if (dragHandle < 3) {
        // Single-axis drag.
        float t = axisParam(mouseX, mouseY, ext, view, proj, dragHandle);
        desiredPrimWorld = dragStartPrimWorld + (t - dragStartT) * kAxisDirs[dragHandle];
    } else {
        // Plane drag: ray-plane intersection gives a 2D delta on the plane.
        auto hit = planeHit(mouseX, mouseY, ext, view, proj, dragHandle - 3);
        auto delta = hit - dragStartPlaneHit;
        desiredPrimWorld = dragStartPrimWorld + delta;
    }

    auto newLocal = dragStartLocal;
    newLocal.position = glm::vec3(dragStartParentInv * glm::vec4(desiredPrimWorld, 1.0f));
    return newLocal;
}
