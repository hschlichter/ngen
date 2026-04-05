#pragma once

#include "rhicommandbuffervulkan.h"
#include "rhidevice.h"
#include "rhiresourcesvulkan.h"
#include "rhiswapchainvulkan.h"

#include <vulkan/vulkan.h>

class RhiDeviceVulkan : public RhiDevice {
public:
    auto init(SDL_Window* window) -> std::expected<void, int> override;
    auto destroy() -> void override;
    auto waitIdle() -> void override;

    auto createSwapchain(SDL_Window* window) -> RhiSwapchain* override;
    auto createBuffer(const RhiBufferDesc& desc) -> RhiBuffer* override;
    auto createTexture(const RhiTextureDesc& desc) -> RhiTexture* override;
    auto createSampler(const RhiSamplerDesc& desc) -> RhiSampler* override;
    auto createShaderModule(const char* filepath) -> RhiShaderModule* override;
    auto createGraphicsPipeline(const RhiGraphicsPipelineDesc& desc) -> RhiPipeline* override;
    auto createDescriptorSetLayout(std::span<const RhiDescriptorBinding> bindings) -> RhiDescriptorSetLayout* override;
    auto createDescriptorPool(uint32_t maxSets, std::span<const RhiDescriptorBinding> bindings) -> RhiDescriptorPool* override;
    auto allocateDescriptorSets(RhiDescriptorPool* pool, RhiDescriptorSetLayout* layout, uint32_t count) -> std::vector<RhiDescriptorSet*> override;
    auto updateDescriptorSet(RhiDescriptorSet* set, std::span<const RhiDescriptorWrite> writes) -> void override;

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

    [[nodiscard]] auto vkInstance() const -> VkInstance { return instance; }
    [[nodiscard]] auto vkPhysicalDevice() const -> VkPhysicalDevice { return physicalDevice; }
    [[nodiscard]] auto vkDevice() const -> VkDevice { return device; }
    [[nodiscard]] auto vkGraphicsQueue() const -> VkQueue { return graphicsQueue; }
    [[nodiscard]] auto vkQueueFamilyIndex() const -> uint32_t { return queueFamilyIndex; }
    [[nodiscard]] auto vkCommandPool() const -> VkCommandPool { return cmdPool; }

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

    static auto toVkBufferUsage(RhiBufferUsage usage) -> VkBufferUsageFlags;
    static auto toVkImageUsage(RhiTextureUsage usage) -> VkImageUsageFlags;
    static auto toVkMemoryProps(RhiMemoryUsage usage) -> VkMemoryPropertyFlags;
    static auto toVkShaderStage(RhiShaderStage stage) -> VkShaderStageFlags;

public:
    static auto toVkFormat(RhiFormat format) -> VkFormat;
};
