#pragma once

#include "framegraph.h"
#include "rhitypes.h"

struct ShadowPassData {
    FgTextureHandle shadowMap;
};

// Dummy shadow pass: creates a shadow map transient and clears it. Placeholder for a real
// light-space depth pre-pass.
auto addShadowPass(FrameGraph& fg, RhiExtent2D extent, RhiFormat depthFormat) -> const ShadowPassData&;
