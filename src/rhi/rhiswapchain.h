#pragma once

#include "rhitypes.h"

#include <expected>

class RhiSwapchain {
public:
    RhiSwapchain() = default;
    RhiSwapchain(const RhiSwapchain&) = delete;
    RhiSwapchain& operator=(const RhiSwapchain&) = delete;
    RhiSwapchain(RhiSwapchain&&) = default;
    RhiSwapchain& operator=(RhiSwapchain&&) = default;
    virtual ~RhiSwapchain() = default;

    virtual auto destroy() -> void = 0;
    virtual auto acquireNextImage(RhiSemaphore* signalSemaphore) -> std::expected<uint32_t, int> = 0;
    virtual auto imageCount() -> uint32_t = 0;
    virtual auto extent() -> RhiExtent2D = 0;
    virtual auto image(uint32_t index) -> RhiTexture* = 0;
    virtual auto depthImage() -> RhiTexture* = 0;
    virtual auto colorFormat() -> RhiFormat = 0;
    virtual auto depthFormat() -> RhiFormat = 0;
};
