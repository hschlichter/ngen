#pragma once

#include "framegraph.h"

// Declares the swapchain image as "being presented" so the frame graph emits the
// ColorAttachment/Transfer → PresentSrc layout transition. No GPU work.
void addPresentPass(FrameGraph& fg, FgTextureHandle target);
