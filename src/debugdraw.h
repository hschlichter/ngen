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
    auto newFrame() -> void;
    auto data() const -> const DebugDrawData& { return frameData; }

private:
    DebugDrawData frameData;
};
