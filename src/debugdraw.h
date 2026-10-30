#pragma once

#include "scenetypes.h"

#include <glm/glm.hpp>
#include <utility>
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
    // A box in a space `toWorld` places (rotated, scaled), drawn by its twelve edges.
    auto orientedBox(const glm::mat4& toWorld, glm::vec3 min, glm::vec3 max, glm::vec4 color) -> void;
    // A circle around `center` in the plane of the unit axes `a` and `b`.
    auto circle(glm::vec3 center, glm::vec3 a, glm::vec3 b, float radius, glm::vec4 color, int segments = 48) -> void;
    auto grid(glm::vec3 cameraPos, glm::vec3 worldUp, float spacing, int halfCount, glm::vec4 color) -> void;
    // Sun-style gizmo for a directional light: disc perpendicular to `outgoingDir`, a short
    // ring of rays radiating outward, and a longer shaft along `outgoingDir` showing where
    // the light is shining to. `origin` is the light's world position; `outgoingDir` should
    // be the unit vector the light rays travel along (opposite of "toward the sun").
    auto sunLight(glm::vec3 origin, glm::vec3 outgoingDir, float radius, float shaftLen, glm::vec4 color) -> void;
    auto newFrame() -> void;
    auto data() const -> const DebugDrawData& { return frameData; }
    // The frame's lines, moved out: the list is empty afterwards, until the next frame draws. The next frame reserves as many lines.
    auto take() -> DebugDrawData {
        lastLineCount = frameData.lines.size();
        return std::exchange(frameData, {});
    }

private:
    DebugDrawData frameData;
    size_t lastLineCount = 0;
};
