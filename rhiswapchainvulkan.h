#pragma once

#include "rhiswapchain.h"
#include "rhiresourcesvulkan.h"

#include <vulkan/vulkan.h>
#include <vector>

struct SDL_Window;

class RhiSwapchainVulkan : public RhiSwapchain {
    friend class RhiDeviceVulkan;
public:
    int init(VkPhysicalDevice physicalDevice, VkDevice device, VkSurfaceKHR surface,
             uint32_t queueFamilyIndex, SDL_Window* window);
    void destroy() override;

    int acquireNextImage(RhiSemaphore* signalSemaphore, uint32_t* outIndex) override;
    uint32_t imageCount() override { return imgCount; }
    RhiExtent2D extent() override { return ext; }
    RhiRenderPass* renderPass() override { return &rhiRenderPass; }
    RhiFramebuffer* framebuffer(uint32_t index) override { return &rhiFramebuffers[index]; }

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

    uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter,
                            VkMemoryPropertyFlags properties);
};
