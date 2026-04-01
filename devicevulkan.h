#pragma once

#include <vulkan/vulkan.h>

struct SDL_Window;

class DeviceVulkan {
public:
    DeviceVulkan() = default;
    DeviceVulkan(const DeviceVulkan&) = delete;
    DeviceVulkan& operator=(const DeviceVulkan&) = delete;
    ~DeviceVulkan() = default;

    int init(SDL_Window* window);
    void destroy();

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    int createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                     VkBuffer* buffer, VkDeviceMemory* memory);
    int copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);
    void transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);
    VkShaderModule loadShaderModule(const char* filepath);

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = UINT32_MAX;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
};
