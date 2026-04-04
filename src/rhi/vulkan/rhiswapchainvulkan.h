#pragma once

#include "rhiresourcesvulkan.h"
#include "rhiswapchain.h"

#include <expected>
#include <vector>
#include <vulkan/vulkan.h>

struct SDL_Window;

class RhiSwapchainVulkan : public RhiSwapchain {
    friend class RhiDeviceVulkan;

public:
    auto init(VkPhysicalDevice physicalDevice, VkDevice device, VkSurfaceKHR surface, uint32_t queueFamilyIndex, SDL_Window* window)
        -> std::expected<void, int>;
    auto destroy() -> void override;

    auto acquireNextImage(RhiSemaphore* signalSemaphore) -> std::expected<uint32_t, int> override;
    auto imageCount() -> uint32_t override { return imgCount; }
    auto extent() -> RhiExtent2D override { return ext; }
    auto image(uint32_t index) -> RhiTexture* override { return &colorImages[index]; }
    auto depthImage() -> RhiTexture* override { return &rhiDepthImage; }
    auto colorFormat() -> RhiFormat override { return rhiColorFormat; }
    auto depthFormat() -> RhiFormat override { return RhiFormat::D32_SFLOAT; }

private:
    VkDevice vkDevice = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    RhiExtent2D ext = {};
    uint32_t imgCount = 0;
    RhiFormat rhiColorFormat = RhiFormat::Undefined;

    std::vector<RhiTextureVulkan> colorImages;

    VkImage vkDepthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    RhiTextureVulkan rhiDepthImage;

    static auto findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) -> uint32_t;
    static auto vkFormatToRhiFormat(VkFormat format) -> RhiFormat;
};
