#pragma once

#include <cstdint>

// Culling window: the frustum culling toggle, the frozen frustum for inspecting what a
// camera pose culls, the red/green AABB overlay, and this frame's counts
// (docs/plan_frustum_culling.md). The flags are the EditorUI's; session verbs write the same.
struct CullingWindowInputs {
    bool& enabled;
    bool& frozen;
    bool& showCulled;
    uint32_t instances = 0;
    uint32_t culled = 0;
};

void drawCullingWindow(bool& show, CullingWindowInputs in);
