#include "axis3dgizmo.h"
#include "camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

static constexpr glm::vec3 axisDirs[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static constexpr glm::vec4 axisColors[3] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0.4f, 1, 1}};

Axis3DGizmo::Axis3DGizmo(Camera* camera) : camera(camera) {
}

auto Axis3DGizmo::draw(RhiExtent2D fullExtent, const glm::mat4& viewMatrix) -> GizmoDrawRequest {
    auto rotView = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -4.0f)) * glm::mat4(glm::mat3(viewMatrix));
    auto proj = glm::perspective(glm::radians(30.0f), 1.0f, 0.1f, 100.0f);
    proj[1][1] *= -1.0f;

    lastViewProj = proj * rotView;
    lastViewMatrix = viewMatrix;
    lastFullExtent = fullExtent;

    frameVertices.clear();

    for (int i = 0; i < 3; i++) {
        auto dir = axisDirs[i];
        auto color = axisColors[i];
        auto origin = glm::vec3(0.0f);
        auto tip = dir;

        frameVertices.push_back({origin, color});
        frameVertices.push_back({tip, color});

        if (i == hoveredAxis) {
            // Draw extra offset lines to make it look thicker
            constexpr float t = 0.03f;
            for (int a = 0; a < 3; a++) {
                if (a == i) {
                    continue;
                }
                glm::vec3 offset(0.0f);
                offset[a] = t;
                frameVertices.push_back({origin + offset, color});
                frameVertices.push_back({tip + offset, color});
                frameVertices.push_back({origin - offset, color});
                frameVertices.push_back({tip - offset, color});
            }
        }
    }

    auto vpX = (int32_t) (fullExtent.width - gizmoSize - margin);
    auto vpY = (int32_t) margin;

    return {
        .vertices = frameVertices,
        .viewProj = lastViewProj,
        .vpX = vpX,
        .vpY = vpY,
        .vpExtent = {gizmoSize, gizmoSize},
    };
}

auto Axis3DGizmo::updateHover(float mouseX, float mouseY) -> void {
    hoveredAxis = findClosestAxis(mouseX, mouseY);
}

auto Axis3DGizmo::findClosestAxis(float mouseX, float mouseY) const -> int {
    auto vpX = (float) (lastFullExtent.width - gizmoSize - margin);
    auto vpY = (float) margin;

    if (mouseX < vpX || mouseX > vpX + gizmoSize || mouseY < vpY || mouseY > vpY + gizmoSize) {
        return -1;
    }

    auto projToScreen = [&](glm::vec3 pt) -> glm::vec2 {
        auto clip = lastViewProj * glm::vec4(pt, 1.0f);
        auto ndc = glm::vec3(clip) / clip.w;
        return {vpX + (ndc.x * 0.5f + 0.5f) * gizmoSize, vpY + (ndc.y * 0.5f + 0.5f) * gizmoSize};
    };

    auto mouse = glm::vec2(mouseX, mouseY);
    auto origin = projToScreen({0, 0, 0});

    auto distToSegment = [](glm::vec2 p, glm::vec2 a, glm::vec2 b) -> float {
        auto ab = b - a;
        auto ap = p - a;
        float t = glm::clamp(glm::dot(ap, ab) / glm::dot(ab, ab), 0.0f, 1.0f);
        auto closest = a + t * ab;
        return glm::length(p - closest);
    };

    float bestDist = 15.0f;
    int bestAxis = -1;

    for (int i = 0; i < 3; i++) {
        auto tip = projToScreen(axisDirs[i]);
        float dist = distToSegment(mouse, origin, tip);
        if (dist < bestDist) {
            bestDist = dist;
            bestAxis = i;
        }
    }

    return bestAxis;
}

auto Axis3DGizmo::hitTest(float mouseX, float mouseY, RhiExtent2D windowExtent) -> bool {
    int bestAxis = findClosestAxis(mouseX, mouseY);
    if (bestAxis < 0) {
        return false;
    }

    auto vpX = (float) (lastFullExtent.width - gizmoSize - margin);
    auto vpY = (float) margin;

    auto projToScreen = [&](glm::vec3 pt) -> glm::vec2 {
        auto clip = lastViewProj * glm::vec4(pt, 1.0f);
        auto ndc = glm::vec3(clip) / clip.w;
        return {vpX + (ndc.x * 0.5f + 0.5f) * gizmoSize, vpY + (ndc.y * 0.5f + 0.5f) * gizmoSize};
    };

    // Snap to whichever direction the camera is already closest to looking along
    auto camForward = camera->forward();
    float d = glm::dot(camForward, axisDirs[bestAxis]);
    bool negative = d < 0.0f;

    camera->snapToAxis(bestAxis, negative);
    return true;
}
