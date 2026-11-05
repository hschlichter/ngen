#pragma once

#include "rhitypes.h"

#include <glm/glm.hpp>
#include <span>

struct GizmoVertex {
    glm::vec3 position;
    glm::vec4 color;
};

struct GizmoDrawRequest {
    std::span<const GizmoVertex> vertices;
    glm::mat4 viewProj;
    int32_t vpX = 0;
    int32_t vpY = 0;
    RhiExtent2D vpExtent = {};
};
