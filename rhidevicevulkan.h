#pragma once

#include "rhicommandbuffervulkan.h"
#include "rhidevice.h"
#include "rhiresourcesvulkan.h"
#include "rhiswapchainvulkan.h"

#include <vulkan/vulkan.h>

class RhiDeviceVulkan : public RhiDevice {
public:
    int init(SDL_Window* window) override;
    void destroy() override;
    void waitIdle() override;

    RhiSwapchain* createSwapchain(SDL_Window* window) override;
    RhiBuffer* createBuffer(const RhiBufferDesc& desc) override;
    RhiTexture* createTexture(const RhiTextureDesc& desc) override;
    RhiSampler* createSampler(const RhiSamplerDesc& desc) override;
    RhiShaderModule* createShaderModule(const char* filepath) override;
    RhiPipeline* createGraphicsPipeline(const RhiGraphicsPipelineDesc& desc) override;
    RhiDescriptorSetLayout* createDescriptorSetLayout(const RhiDescriptorBinding* bindings, uint32_t count) override;
    RhiDescriptorPool* createDescriptorPool(uint32_t maxSets, const RhiDescriptorBinding* bindings, uint32_t bindingCount) override;
    std::vector<RhiDescriptorSet*> allocateDescriptorSets(RhiDescriptorPool* pool, RhiDescriptorSetLayout* layout, uint32_t count) override;
    void updateDescriptorSet(RhiDescriptorSet* set, const RhiDescriptorWrite* writes, uint32_t writeCount) override;

    RhiCommandBuffer* createCommandBuffer() override;
    RhiSemaphore* createSemaphore() override;
    RhiFence* createFence(bool signaled) override;

    void waitForFence(RhiFence* fence) override;
    void resetFence(RhiFence* fence) override;
    void submitCommandBuffer(RhiCommandBuffer* cmd, const RhiSubmitInfo& info) override;
    void present(RhiSwapchain* swapchain, RhiSemaphore* waitSemaphore, uint32_t imageIndex) override;

    void* mapBuffer(RhiBuffer* buffer) override;
    void unmapBuffer(RhiBuffer* buffer) override;
    void copyBuffer(RhiBuffer* src, RhiBuffer* dst, uint64_t size) override;

    void destroyBuffer(RhiBuffer* buffer) override;
    void destroyTexture(RhiTexture* texture) override;
    void destroySampler(RhiSampler* sampler) override;
    void destroyShaderModule(RhiShaderModule* module) override;
    void destroyPipeline(RhiPipeline* pipeline) override;
    void destroyDescriptorSetLayout(RhiDescriptorSetLayout* layout) override;
    void destroyDescriptorPool(RhiDescriptorPool* pool) override;
    void destroySemaphore(RhiSemaphore* semaphore) override;
    void destroyFence(RhiFence* fence) override;
    void destroyCommandBuffer(RhiCommandBuffer* cmd) override;

private:
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = UINT32_MAX;
    VkCommandPool cmdPool = VK_NULL_HANDLE;

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    void transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);

    VkBufferUsageFlags toVkBufferUsage(RhiBufferUsage usage);
    VkMemoryPropertyFlags toVkMemoryProps(RhiMemoryUsage usage);
    VkFormat toVkFormat(RhiFormat format);
    VkShaderStageFlags toVkShaderStage(RhiShaderStage stage);
};
