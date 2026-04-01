#pragma once

#include <vulkan/vulkan.h>
#include <vector>

class DeviceVulkan;
struct SDL_Window;

class SwapchainVulkan {
public:
    SwapchainVulkan() = default;
    SwapchainVulkan(const SwapchainVulkan&) = delete;
    SwapchainVulkan& operator=(const SwapchainVulkan&) = delete;
    ~SwapchainVulkan() = default;

    int init(DeviceVulkan& dev, SDL_Window* window);
    void destroy(VkDevice device);

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat imageFormat;
    VkExtent2D extent;
    uint32_t imageCount = 0;
    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;

    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthImageView = VK_NULL_HANDLE;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
};
