#pragma once

#include "rhitypes.h"

#include <expected>

class RhiSwapchain {
public:
    virtual ~RhiSwapchain() = default;

    virtual auto destroy() -> void = 0;
    virtual auto acquireNextImage(RhiSemaphore* signalSemaphore) -> std::expected<uint32_t, int> = 0;
    virtual auto imageCount() -> uint32_t = 0;
    virtual auto extent() -> RhiExtent2D = 0;
    virtual auto renderPass() -> RhiRenderPass* = 0;
    virtual auto framebuffer(uint32_t index) -> RhiFramebuffer* = 0;
};
