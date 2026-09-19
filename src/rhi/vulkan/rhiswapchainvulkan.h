#pragma once

#include "rhiresourcesvulkan.h"
#include "rhiswapchain.h"

#include <expected>
#include <vector>
#include <vulkan/vulkan.h>

class RhiSwapchainVulkan : public RhiSwapchain {
    friend class RhiDeviceVulkan;

public:
    auto init(VkPhysicalDevice physicalDevice, VkDevice device, VkSurfaceKHR surface, uint32_t queueFamilyIndex, RhiExtent2D extent) -> std::expected<void, RhiError>;
    auto destroy() -> void override;
    auto recreate(RhiExtent2D extent) -> std::expected<void, RhiError> override;

    auto acquireNextImage(RhiSemaphore* signalSemaphore) -> std::expected<uint32_t, RhiError> override;
    auto imageCount() -> uint32_t override { return imgCount; }
    auto extent() -> RhiExtent2D override { return ext; }
    auto image(uint32_t index) -> RhiTexture* override { return &colorImages[index]; }
    auto colorFormat() -> RhiFormat override { return rhiColorFormat; }

private:
    VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkSurfaceKHR vkSurface = VK_NULL_HANDLE;
    uint32_t vkQueueFamilyIndex = 0;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    RhiExtent2D ext = {};
    uint32_t imgCount = 0;
    RhiFormat rhiColorFormat = RhiFormat::Undefined;

    std::vector<RhiTextureVulkan> colorImages;

    static auto vkFormatToRhiFormat(VkFormat format) -> RhiFormat;
};
