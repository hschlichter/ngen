#pragma once

#include "rhitypes.h"

class RhiSwapchain {
public:
    virtual ~RhiSwapchain() = default;

    virtual auto destroy() -> void = 0;
    virtual auto acquireNextImage(RhiSemaphore* signalSemaphore, uint32_t* outIndex) -> int = 0;
    virtual auto imageCount() -> uint32_t = 0;
    virtual auto extent() -> RhiExtent2D = 0;
    virtual auto renderPass() -> RhiRenderPass* = 0;
    virtual auto framebuffer(uint32_t index) -> RhiFramebuffer* = 0;
};
