#pragma once

#include "rhicommandbuffervulkan.h"
#include "rhidevice.h"
#include "rhiresourcesvulkan.h"
#include "rhiswapchainvulkan.h"

#include <vulkan/vulkan.h>

class RhiDeviceVulkan : public RhiDevice {
public:
    auto init(SDL_Window* window) -> int override;
    auto destroy() -> void override;
    auto waitIdle() -> void override;

    auto createSwapchain(SDL_Window* window) -> RhiSwapchain* override;
    auto createBuffer(const RhiBufferDesc& desc) -> RhiBuffer* override;
    auto createTexture(const RhiTextureDesc& desc) -> RhiTexture* override;
    auto createSampler(const RhiSamplerDesc& desc) -> RhiSampler* override;
    auto createShaderModule(const char* filepath) -> RhiShaderModule* override;
    auto createGraphicsPipeline(const RhiGraphicsPipelineDesc& desc) -> RhiPipeline* override;
    auto createDescriptorSetLayout(const RhiDescriptorBinding* bindings, uint32_t count) -> RhiDescriptorSetLayout* override;
    auto createDescriptorPool(uint32_t maxSets, const RhiDescriptorBinding* bindings, uint32_t bindingCount) -> RhiDescriptorPool* override;
    auto allocateDescriptorSets(RhiDescriptorPool* pool, RhiDescriptorSetLayout* layout, uint32_t count) -> std::vector<RhiDescriptorSet*> override;
    auto updateDescriptorSet(RhiDescriptorSet* set, const RhiDescriptorWrite* writes, uint32_t writeCount) -> void override;

    auto createCommandBuffer() -> RhiCommandBuffer* override;
    auto createSemaphore() -> RhiSemaphore* override;
    auto createFence(bool signaled) -> RhiFence* override;

    auto waitForFence(RhiFence* fence) -> void override;
    auto resetFence(RhiFence* fence) -> void override;
    auto submitCommandBuffer(RhiCommandBuffer* cmd, const RhiSubmitInfo& info) -> void override;
    auto present(RhiSwapchain* swapchain, RhiSemaphore* waitSemaphore, uint32_t imageIndex) -> void override;

    auto mapBuffer(RhiBuffer* buffer) -> void* override;
    auto unmapBuffer(RhiBuffer* buffer) -> void override;
    auto copyBuffer(RhiBuffer* src, RhiBuffer* dst, uint64_t size) -> void override;

    auto destroyBuffer(RhiBuffer* buffer) -> void override;
    auto destroyTexture(RhiTexture* texture) -> void override;
    auto destroySampler(RhiSampler* sampler) -> void override;
    auto destroyShaderModule(RhiShaderModule* module) -> void override;
    auto destroyPipeline(RhiPipeline* pipeline) -> void override;
    auto destroyDescriptorSetLayout(RhiDescriptorSetLayout* layout) -> void override;
    auto destroyDescriptorPool(RhiDescriptorPool* pool) -> void override;
    auto destroySemaphore(RhiSemaphore* semaphore) -> void override;
    auto destroyFence(RhiFence* fence) -> void override;
    auto destroyCommandBuffer(RhiCommandBuffer* cmd) -> void override;

private:
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = UINT32_MAX;
    VkCommandPool cmdPool = VK_NULL_HANDLE;

    auto findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) -> uint32_t;
    auto transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) -> void;

    auto toVkBufferUsage(RhiBufferUsage usage) -> VkBufferUsageFlags;
    auto toVkMemoryProps(RhiMemoryUsage usage) -> VkMemoryPropertyFlags;
    auto toVkFormat(RhiFormat format) -> VkFormat;
    auto toVkShaderStage(RhiShaderStage stage) -> VkShaderStageFlags;
};
