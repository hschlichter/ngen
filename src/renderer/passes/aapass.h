#pragma once

#include "framegraph.h"
#include "rhitypes.h"

struct AAPassData {
    FgTextureHandle sceneColor;
    FgTextureHandle sceneColorAA;
};

// Dummy AA pass: reads the scene color and produces sceneColorAA by blitting. A real
// implementation would sample sceneColor in a shader and write to sceneColorAA as a
// color attachment.
auto addAAPass(FrameGraph& fg, FgTextureHandle input, RhiExtent2D extent, RhiFormat format) -> const AAPassData&;
