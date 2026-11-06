#pragma once

#include <glm/glm.hpp>

// The geometry set's binding 0 (shaders/meshlet.glsl): the rendered camera, and the culling camera the depth prepass draws from while it
// isn't the rendered one.
struct UniformBufferObject {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 cullViewProj;
};
