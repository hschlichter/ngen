#pragma once

#include "drawlists.h"

// Culling window: the culling switches, the frozen frustum for inspecting what a camera pose culls, the red/green AABB overlay, the
// meshlet culling debug view, and the latest counts read back from the GPU culling, in its steps: the scene, what instance culling
// keeps, what meshlet culling draws. The flags are the UI's; session verbs write the same.
struct CullingWindowInputs {
    bool& enabled;
    bool& frozen;
    bool& occlusion;
    bool& showCulled;
    int& overlayView; // view the AABB overlay shows: 0 camera, 1.. cascades
    bool& showCascadeFrusta;
    bool& meshletCullView; // the debug view colouring meshlets by the test that culls them
    bool depthPrepass;     // render.prepass: occlusion's phases draw in the depth prepass, not the geometry pass
    const CullResult& cull;
};

void drawCullingWindow(bool& show, CullingWindowInputs in);
