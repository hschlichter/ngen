#include "projection.h"

#include <algorithm>
#include <cmath>

auto perspectiveProjection(float fovY, float aspect) -> glm::mat4 {
    float f = 1.0f / std::tan(fovY * 0.5f);
    // Clip z is the near distance and w the view depth, so depth is near / distance.
    glm::mat4 proj(0.0f);
    proj[0][0] = f / aspect;
    proj[1][1] = -f; // Vulkan's y points down
    proj[2][3] = -1.0f;
    proj[3][2] = projectionNear;
    return proj;
}

auto projectionDepthAt(float distance) -> float {
    return projectionNear / std::max(distance, projectionNear);
}
