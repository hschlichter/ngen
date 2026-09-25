#pragma once

#include "shadowcascades.h"

#include <array>
#include <cstdint>

// Culling window: the frustum culling toggle, the frozen frustum for inspecting what a
// camera pose culls, the red/green AABB overlay, and the latest counts read back from
// the GPU culling. The flags are the EditorUI's; session verbs write the same.
struct CullingWindowInputs {
    bool& enabled;
    bool& frozen;
    bool& showCulled;
    uint32_t instances = 0;
    uint32_t culled = 0;
    uint32_t cascades = 0; // shadow cascades this frame
    std::array<uint32_t, maxShadowCascades> shadowCulled = {};
    std::array<uint32_t, maxShadowCascades> shadowDrawn = {};
};

void drawCullingWindow(bool& show, CullingWindowInputs in);
