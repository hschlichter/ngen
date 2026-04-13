#include "translategizmo.h"

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
    // While dragging, keep the gizmo anchored at the drag origin so it doesn't
    // chase the prim (which lags due to async scene updates).
    this->origin = isDragging() ? dragStartWorld : originWorld;

    // Constant pixel length: derive world scale from camera distance + vertical FOV.
    // proj[1][1] = ±cot(fov/2); sign depends on Vulkan Y-flip — magnitude is what we want.
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
        auto color = (i == hoveredAxis || i == dragAxis) ? kHotColor : kAxisColors[i];
        frameVertices.push_back({this->origin, color});
        frameVertices.push_back({this->origin + kAxisDirs[i] * this->scale, color});
    }
}

auto TranslateGizmo::findClosestAxis(float mouseX, float mouseY) const -> int {
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

auto TranslateGizmo::axisParam(float mouseX, float mouseY, RhiExtent2D ext, const glm::mat4& view, const glm::mat4& proj, int axis) const -> float {
    // Closest-point parameter along the axis line through dragStartWorld for the
    // mouse pick ray. Standard skew-line solve.
    auto [ro, rd] = mouseRay(mouseX, mouseY, ext, view, proj);
    auto ad = kAxisDirs[axis];
    auto w0 = dragStartWorld - ro;
    float b = glm::dot(ad, rd);
    float denom = 1.0f - b * b; // a = c = 1 since both dirs unit
    if (std::abs(denom) < 1e-6f) {
        return dragStartT; // ray parallel to axis
    }
    return (b * glm::dot(rd, w0) - glm::dot(ad, w0)) / denom;
}

auto TranslateGizmo::tryGrab(
    float mouseX, float mouseY, RhiExtent2D ext, const glm::mat4& view, const glm::mat4& proj, const Transform& currentLocal, const glm::mat4& currentWorld)
    -> bool {
    if (!visible) {
        return false;
    }
    int axis = findClosestAxis(mouseX, mouseY);
    if (axis < 0) {
        return false;
    }
    dragAxis = axis;
    dragStartLocal = currentLocal;
    dragStartWorld = glm::vec3(currentWorld[3]);
    dragStartParentInv = glm::inverse(currentWorld * glm::inverse(currentLocal.toMat4()));
    dragStartT = axisParam(mouseX, mouseY, ext, view, proj, axis);
    return true;
}

auto TranslateGizmo::dragUpdate(float mouseX, float mouseY, RhiExtent2D ext, const glm::mat4& view, const glm::mat4& proj) const -> std::optional<Transform> {
    if (!isDragging()) {
        return std::nullopt;
    }
    float t = axisParam(mouseX, mouseY, ext, view, proj, dragAxis);
    auto desiredWorld = dragStartWorld + (t - dragStartT) * kAxisDirs[dragAxis];
    auto newLocal = dragStartLocal;
    newLocal.position = glm::vec3(dragStartParentInv * glm::vec4(desiredWorld, 1.0f));
    return newLocal;
}
