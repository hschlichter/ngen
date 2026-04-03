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
    auto renderPass() -> RhiRenderPass* override { return &rhiRenderPass; }
    auto framebuffer(uint32_t index) -> RhiFramebuffer* override { return &rhiFramebuffers[index]; }

private:
    VkDevice vkDevice = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    RhiExtent2D ext = {};
    uint32_t imgCount = 0;
    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;

    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthImageView = VK_NULL_HANDLE;

    RhiRenderPassVulkan rhiRenderPass;
    std::vector<RhiFramebufferVulkan> rhiFramebuffers;

    static auto findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) -> uint32_t;
};
