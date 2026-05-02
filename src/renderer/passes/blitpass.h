#pragma once

#include "framegraph.h"
#include "rhitypes.h"

void addBlitPass(FrameGraph& fg, const char* name, FgTextureHandle src, FgTextureHandle dst, RhiExtent2D srcExtent, RhiExtent2D dstExtent);
