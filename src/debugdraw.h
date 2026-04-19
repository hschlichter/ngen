#pragma once

#include "scenetypes.h"

#include <glm/glm.hpp>
#include <vector>

struct DebugVertex {
    glm::vec3 position;
    glm::vec4 color;
};

struct DebugDrawData {
    std::vector<DebugVertex> lines;
};

class DebugDraw {
public:
    auto line(glm::vec3 a, glm::vec3 b, glm::vec4 color) -> void;
    auto box(const AABB& box, glm::vec4 color) -> void;
    auto sphere(glm::vec3 center, float radius, glm::vec4 color, int segments = 24) -> void;
    auto grid(glm::vec3 cameraPos, glm::vec3 worldUp, float spacing, int halfCount, glm::vec4 color) -> void;
    // Sun-style gizmo for a directional light: disc perpendicular to `outgoingDir`, a short
    // ring of rays radiating outward, and a longer shaft along `outgoingDir` showing where
    // the light is shining to. `origin` is the light's world position; `outgoingDir` should
    // be the unit vector the light rays travel along (opposite of "toward the sun").
    auto sunLight(glm::vec3 origin, glm::vec3 outgoingDir, float radius, float shaftLen, glm::vec4 color) -> void;
    auto newFrame() -> void;
    auto data() const -> const DebugDrawData& { return frameData; }

private:
    DebugDrawData frameData;
};
