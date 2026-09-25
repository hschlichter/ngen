#pragma once

#include "renderworld.h"
#include "scenetypes.h"
#include "shadowcascades.h"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <vector>

// Culling toggles and the frozen frustum. The culling itself runs on the GPU
// (docs/plan_gpu_culling.md); the snapshot carries the matrix update() returns.
struct CullState {
    bool enabled = true;
    bool frozen = false;       // set from the editor; the frustum stops following the camera
    bool frozenActive = false; // true once a frozen matrix has been captured
    glm::mat4 frozenViewProj = glm::mat4(1.0f);

    // Picks the live or frozen matrix for this frame, capturing the live one on the frame
    // freezing starts. Returns the matrix the cull test uses.
    auto update(const glm::mat4& liveViewProj) -> glm::mat4;

    // Corners of the frozen frustum for the debug overlay; only meaningful while frozenActive.
    auto frozenCorners() const -> std::array<glm::vec3, 8>;
};
