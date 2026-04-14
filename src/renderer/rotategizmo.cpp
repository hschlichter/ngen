#include "rotategizmo.h"

#include <cmath>

static constexpr glm::vec3 kAxisDirs[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static constexpr glm::vec4 kAxisColors[3] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0.4f, 1, 1}};
static constexpr glm::vec4 kHotColor = {1, 1, 0, 1};
static constexpr float kPixelLength = 90.0f;
static constexpr float kHitThresholdPx = 12.0f;
static constexpr int kSegments = 48;

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

// Circle sample point for axis `i` at angle `theta`, centered at `center` with `radius`.
static auto circlePoint(int axis, float theta, const glm::vec3& center, float radius) -> glm::vec3 {
    auto perpA = kAxisDirs[(axis + 1) % 3];
    auto perpB = kAxisDirs[(axis + 2) % 3];
    return center + (perpA * std::cos(theta) + perpB * std::sin(theta)) * radius;
}

auto RotateGizmo::update(RhiExtent2D ext,
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

    constexpr float step = 2.0f * glm::pi<float>() / (float) kSegments;
    for (int i = 0; i < 3; i++) {
        auto color = (i == hoveredAxis || i == dragAxis) ? kHotColor : kAxisColors[i];
        for (int s = 0; s < kSegments; s++) {
            float t0 = (float) s * step;
            float t1 = (float) (s + 1) * step;
            frameVertices.push_back({circlePoint(i, t0, this->origin, this->scale), color});
            frameVertices.push_back({circlePoint(i, t1, this->origin, this->scale), color});
        }
    }
}

auto RotateGizmo::findClosestAxis(float mouseX, float mouseY) const -> int {
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
    constexpr float step = 2.0f * glm::pi<float>() / (float) kSegments;
    float bestDist = kHitThresholdPx;
    int best = -1;

    for (int i = 0; i < 3; i++) {
        for (int s = 0; s < kSegments; s++) {
            auto a = toScreen(circlePoint(i, (float) s * step, origin, scale));
            auto b = toScreen(circlePoint(i, (float) (s + 1) * step, origin, scale));
            float d = distToSeg(mouse, a, b);
            if (d < bestDist) {
                bestDist = d;
                best = i;
            }
        }
    }
    return best;
}

auto RotateGizmo::planeAngle(float mouseX, float mouseY, RhiExtent2D ext, const glm::mat4& view, const glm::mat4& proj, int axis) const -> float {
    auto [ro, rd] = mouseRay(mouseX, mouseY, ext, view, proj);
    auto normal = kAxisDirs[axis];
    float denom = glm::dot(rd, normal);
    if (std::abs(denom) < 1e-6f) {
        return dragStartAngle; // ray parallel to plane
    }
    float t = glm::dot(dragStartAnchor - ro, normal) / denom;
    auto hit = ro + rd * t;
    auto local = hit - dragStartAnchor;
    auto perpA = kAxisDirs[(axis + 1) % 3];
    auto perpB = kAxisDirs[(axis + 2) % 3];
    return std::atan2(glm::dot(local, perpB), glm::dot(local, perpA));
}

auto RotateGizmo::tryGrab(float mouseX,
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

    // Extract parent world rotation (for converting world-space rotation delta to local).
    auto parentWorld = currentWorld * glm::inverse(currentLocal.toMat4());
    auto m = glm::mat3(parentWorld);
    m[0] = glm::normalize(m[0]);
    m[1] = glm::normalize(m[1]);
    m[2] = glm::normalize(m[2]);
    dragStartParentWorldRot = glm::quat_cast(m);

    dragStartAngle = planeAngle(mouseX, mouseY, ext, view, proj, axis);
    return true;
}

auto RotateGizmo::dragUpdate(float mouseX, float mouseY, RhiExtent2D ext, const glm::mat4& view, const glm::mat4& proj) const -> std::optional<Transform> {
    if (!isDragging()) {
        return std::nullopt;
    }
    float angle = planeAngle(mouseX, mouseY, ext, view, proj, dragAxis);
    float delta = angle - dragStartAngle;

    // World-space rotation delta around the active axis.
    auto worldDelta = glm::angleAxis(delta, kAxisDirs[dragAxis]);
    // Convert to local space: localDelta = inv(parentRot) * worldDelta * parentRot.
    auto localDelta = glm::inverse(dragStartParentWorldRot) * worldDelta * dragStartParentWorldRot;

    auto newLocal = dragStartLocal;
    newLocal.rotation = localDelta * dragStartLocal.rotation;
    return newLocal;
}
