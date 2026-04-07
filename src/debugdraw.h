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
    auto newFrame() -> void;
    auto data() const -> const DebugDrawData& { return frameData; }

private:
    DebugDrawData frameData;
};
