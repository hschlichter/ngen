#pragma once

#include "rhitypes.h"

class RhiSwapchain {
public:
    virtual ~RhiSwapchain() = default;

    virtual void destroy() = 0;
    virtual int acquireNextImage(RhiSemaphore* signalSemaphore, uint32_t* outIndex) = 0;
    virtual uint32_t imageCount() = 0;
    virtual RhiExtent2D extent() = 0;
    virtual RhiRenderPass* renderPass() = 0;
    virtual RhiFramebuffer* framebuffer(uint32_t index) = 0;
};
