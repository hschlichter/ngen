#pragma once

#include <glm/glm.hpp>

struct UniformBufferObject {
    glm::mat4 view;
    glm::mat4 proj;
};

// Draws with at least this many indices get their own GPU profiling zone inside a pass.
inline constexpr uint32_t largeDrawIndexCount = 100000;
