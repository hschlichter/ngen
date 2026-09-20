#pragma once

#include "renderworld.h"
#include "scenetypes.h"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <vector>

// Per-instance frustum culling, run on the main thread while the snapshot is built
// (docs/plan_frustum_culling.md). The result travels to the render thread in
// RenderSnapshot::visible, aligned with RenderWorld::meshInstances.
struct CullState {
    bool enabled = true;
    bool frozen = false;      // set from the editor; the frustum stops following the camera
    bool frozenActive = false; // true once a frozen matrix has been captured
    glm::mat4 frozenViewProj = glm::mat4(1.0f);

    // Picks the live or frozen matrix for this frame, capturing the live one on the frame
    // freezing starts. Returns the matrix the cull test uses.
    auto update(const glm::mat4& liveViewProj) -> glm::mat4;

    // Corners of the frozen frustum for the debug overlay; only meaningful while frozenActive.
    auto frozenCorners() const -> std::array<glm::vec3, 8>;
};

// Fills visible (1 = draw, 0 = culled) for every instance and returns the culled count.
// Instances without valid bounds are always drawn. Leaves visible empty when culling is
// off, which the passes read as "draw everything".
auto cullInstances(const CullState& state, const glm::mat4& viewProj, std::span<const RenderMeshInstance> instances, std::vector<uint8_t>& visible) -> uint32_t;
