#pragma once

#include "rhicommandbuffervulkan.h"
#include "rhidevice.h"
#include "rhiresourcesvulkan.h"
#include "rhiswapchainvulkan.h"

#include <mutex>
#include <unordered_map>
#include <vulkan/vulkan.h>

class RhiDeviceVulkan : public RhiDevice {
public:
    auto init(const RhiWindow& window, const RhiDeviceOptions& options) -> std::expected<void, RhiError> override;
    using RhiDevice::init;
    auto destroy() -> void override;
    auto waitIdle() -> void override;

    auto createSwapchain(RhiExtent2D extent) -> RhiSwapchain* override;
    auto createBuffer(const RhiBufferDesc& desc) -> RhiBuffer* override;
    auto createTexture(const RhiTextureDesc& desc) -> RhiTexture* override;
    auto createSampler(const RhiSamplerDesc& desc) -> RhiSampler* override;
    auto createShaderModule(const RhiShaderDesc& desc) -> RhiShaderModule* override;
    auto createGraphicsPipeline(const RhiGraphicsPipelineDesc& desc) -> RhiPipeline* override;
    auto createComputePipeline(const RhiComputePipelineDesc& desc) -> RhiPipeline* override;
    auto createDescriptorSetLayout(std::span<const RhiDescriptorBinding> bindings) -> RhiDescriptorSetLayout* override;
    auto createDescriptorPool(uint32_t maxSets, std::span<const RhiDescriptorBinding> bindings) -> RhiDescriptorPool* override;
    auto allocateDescriptorSets(RhiDescriptorPool* pool, RhiDescriptorSetLayout* layout, std::span<RhiDescriptorSet*> outSets) -> bool override;
    auto freeDescriptorSets(RhiDescriptorPool* pool, std::span<RhiDescriptorSet* const> sets) -> void override;
    auto updateDescriptorSet(RhiDescriptorSet* set, std::span<const RhiDescriptorWrite> writes) -> void override;

    auto createCommandBuffer() -> RhiCommandBuffer* override;
    auto createQueryPool(uint32_t timestampCount) -> RhiQueryPool* override;
    auto destroyQueryPool(RhiQueryPool* pool) -> void override;
    auto readTimestamps(RhiQueryPool* pool, uint32_t first, std::span<uint64_t> outTicks) -> bool override;
    auto collectGpuZones(RhiCommandBuffer* cmd, std::vector<RhiGpuZone>& out) -> bool override;
    auto collectPipelineStats(RhiCommandBuffer* cmd, std::vector<RhiPipelineStatsZone>& out) -> bool override;
    auto calibrateGpuClock(uint64_t& gpuNs, uint64_t& cpuNs) -> bool override;
    auto createSemaphore() -> RhiSemaphore* override;
    auto createFence(bool signaled) -> RhiFence* override;

    auto waitForFence(RhiFence* fence) -> void override;
    auto resetFence(RhiFence* fence) -> void override;
    auto submitCommandBuffer(RhiCommandBuffer* cmd, const RhiSubmitInfo& info) -> void override;
    auto present(RhiSwapchain* swapchain, RhiSemaphore* waitSemaphore, uint32_t imageIndex) -> std::expected<void, RhiError> override;

    auto mapBuffer(RhiBuffer* buffer) -> void* override;
    auto unmapBuffer(RhiBuffer* buffer) -> void override;

    [[nodiscard]] auto limits() const -> const RhiDeviceLimits& override { return deviceLimits; }
    [[nodiscard]] auto supportsTextureFormat(RhiFormat format, RhiTextureUsageFlags usage) const -> bool override;
    [[nodiscard]] auto validationErrorCount() const -> uint64_t override { return validationErrors; }
    auto onValidationMessage(uint32_t severity, const char* message) -> void;

    auto setDebugName(RhiDebugObject object, const char* name) -> void override;
    auto allocations(std::vector<RhiAllocationInfo>& out) const -> void override;
    auto memoryHeaps(std::vector<RhiMemoryHeapInfo>& out) const -> void override;
    [[nodiscard]] auto describeTransition(RhiTextureState oldState, RhiTextureState newState) const -> RhiTransitionInfo override;
    [[nodiscard]] auto describeTransition(RhiBufferState oldState, RhiBufferState newState) const -> RhiTransitionInfo override;
    [[nodiscard]] auto describeDescriptorSet(const RhiDescriptorSet* set) const -> std::vector<RhiDescriptorInfo> override;

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
    uint32_t queueTimestampValidBits = 0;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    RhiDeviceLimits deviceLimits;
    PFN_vkCmdBeginDebugUtilsLabelEXT cmdBeginLabelFn = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT cmdEndLabelFn = nullptr;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    PFN_vkGetCalibratedTimestampsEXT getCalibratedTimestampsFn = nullptr;
    uint64_t validationErrors = 0;
    uint64_t validationWarnings = 0;

    // Debug names (VK_EXT_debug_utils; null when unavailable) and the allocation registry
    // behind allocations(), keyed by the RHI object.
    PFN_vkSetDebugUtilsObjectNameEXT setObjectNameFn = nullptr;
    bool memoryBudgetAvailable = false;
    VkPhysicalDeviceMemoryProperties memoryProperties = {};
    mutable std::mutex registryMutex;
    std::unordered_map<const void*, RhiAllocationInfo> registry;
    uint64_t nextAllocationSequence = 1;
    auto nameVkObject(VkObjectType type, uint64_t handle, const char* name) -> void;
    auto registerAllocation(const void* object, RhiAllocationInfo info) -> void;
    auto unregisterAllocation(const void* object) -> void;

    auto findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) -> uint32_t;
    auto createPipelineLayout(std::span<RhiDescriptorSetLayout* const> setLayouts, const RhiPushConstantRange& pushConstant) -> VkPipelineLayout;

    static auto toVkBufferUsage(RhiBufferUsageFlags usage) -> VkBufferUsageFlags;
    static auto toVkImageUsage(RhiTextureUsageFlags usage) -> VkImageUsageFlags;
    static auto toVkMemoryProps(RhiMemoryUsage usage) -> VkMemoryPropertyFlags;
    static auto toVkShaderStage(RhiShaderStageFlags stage) -> VkShaderStageFlags;

public:
    static auto toVkFormat(RhiFormat format) -> VkFormat;
};
