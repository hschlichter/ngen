#pragma once

#include "rhitypes.h"
#include "rhiwindow.h"

#include <expected>
#include <span>
#include <vector>

class RhiSwapchain;
class RhiCommandBuffer;

// Runtime options for device creation. Validation is a runtime choice so one
// backend build serves the engine (off by default) and the examples (on when
// asked); the error count turns validation output into a result a program can
// check instead of a log a human must read.
struct RhiDeviceOptions {
    bool enableValidation = false;
};

class RhiDevice {
public:
    RhiDevice() = default;
    RhiDevice(const RhiDevice&) = delete;
    RhiDevice& operator=(const RhiDevice&) = delete;
    RhiDevice(RhiDevice&&) = default;
    RhiDevice& operator=(RhiDevice&&) = default;
    virtual ~RhiDevice() = default;

    virtual auto init(const RhiWindow& window, const RhiDeviceOptions& options = {}) -> std::expected<void, RhiError> = 0;
    virtual auto destroy() -> void = 0;
    virtual auto waitIdle() -> void = 0;

    virtual auto createSwapchain(RhiExtent2D extent) -> RhiSwapchain* = 0;
    virtual auto createBuffer(const RhiBufferDesc& desc) -> RhiBuffer* = 0;
    virtual auto createTexture(const RhiTextureDesc& desc) -> RhiTexture* = 0;
    virtual auto createSampler(const RhiSamplerDesc& desc) -> RhiSampler* = 0;
    virtual auto createShaderModule(const RhiShaderDesc& desc) -> RhiShaderModule* = 0;
    virtual auto createGraphicsPipeline(const RhiGraphicsPipelineDesc& desc) -> RhiPipeline* = 0;
    // Same RhiPipeline type; bindPipeline picks the graphics or compute bind point from it.
    virtual auto createComputePipeline(const RhiComputePipelineDesc& desc) -> RhiPipeline* = 0;
    virtual auto createDescriptorSetLayout(std::span<const RhiDescriptorBinding> bindings) -> RhiDescriptorSetLayout* = 0;
    virtual auto createDescriptorPool(uint32_t maxSets, std::span<const RhiDescriptorBinding> bindings) -> RhiDescriptorPool* = 0;
    // Fills outSets, one set per element. Free them with freeDescriptorSets before destroying the pool.
    virtual auto allocateDescriptorSets(RhiDescriptorPool* pool, RhiDescriptorSetLayout* layout, std::span<RhiDescriptorSet*> outSets) -> bool = 0;
    virtual auto freeDescriptorSets(RhiDescriptorPool* pool, std::span<RhiDescriptorSet* const> sets) -> void = 0;
    virtual auto updateDescriptorSet(RhiDescriptorSet* set, std::span<const RhiDescriptorWrite> writes) -> void = 0;

    virtual auto createCommandBuffer() -> RhiCommandBuffer* = 0;
    // Timestamp queries. Reset on a command buffer before writing; read after the submit's fence.
    // readTimestamps returns false when any requested result is not yet available.
    virtual auto createQueryPool(uint32_t timestampCount) -> RhiQueryPool* = 0;
    virtual auto destroyQueryPool(RhiQueryPool* pool) -> void = 0;
    virtual auto readTimestamps(RhiQueryPool* pool, uint32_t first, std::span<uint64_t> outTicks) -> bool = 0;
    // Simultaneous sample of the GPU timestamp clock (in ns, same unit as RhiGpuZone) and the
    // CPU monotonic clock (ns, std::chrono::steady_clock base). Lets GPU zones be placed on the CPU
    // time axis. False when the device cannot calibrate; callers then anchor GPU work at submit time.
    virtual auto calibrateGpuClock(uint64_t& gpuNs, uint64_t& cpuNs) -> bool = 0;
    // Zones recorded on cmd since its last begin(); valid after the submit's fence. False if not yet available.
    virtual auto collectGpuZones(RhiCommandBuffer* cmd, std::vector<RhiGpuZone>& out) -> bool = 0;
    // Pipeline statistics recorded on cmd since its last begin(); valid after the submit's fence.
    virtual auto collectPipelineStats(RhiCommandBuffer* cmd, std::vector<RhiPipelineStatsZone>& out) -> bool = 0;
    virtual auto createSemaphore() -> RhiSemaphore* = 0;
    virtual auto createFence(bool signaled) -> RhiFence* = 0;

    virtual auto waitForFence(RhiFence* fence) -> void = 0;
    virtual auto resetFence(RhiFence* fence) -> void = 0;
    virtual auto submitCommandBuffer(RhiCommandBuffer* cmd, const RhiSubmitInfo& info) -> void = 0;
    virtual auto present(RhiSwapchain* swapchain, RhiSemaphore* waitSemaphore, uint32_t imageIndex) -> std::expected<void, RhiError> = 0;

    virtual auto mapBuffer(RhiBuffer* buffer) -> void* = 0;
    virtual auto unmapBuffer(RhiBuffer* buffer) -> void = 0;

    [[nodiscard]] virtual auto limits() const -> const RhiDeviceLimits& = 0;
    // Whether textures of this format can be created with every usage bit given.
    // Some formats are optional per backend (e.g. D24_UNORM_S8_UINT on Vulkan).
    [[nodiscard]] virtual auto supportsTextureFormat(RhiFormat format, RhiTextureUsageFlags usage) const -> bool = 0;
    // Errors reported by the backend's validation layer since init; always 0 when validation is off.
    [[nodiscard]] virtual auto validationErrorCount() const -> uint64_t = 0;

    // Debug names: objects created from a desc take desc.debugName; this names or renames any
    // object. Names show in validation messages, external tools and allocations(). Copied.
    virtual auto setDebugName(RhiDebugObject object, const char* name) -> void = 0;
    // Every live buffer and texture, in creation order.
    virtual auto allocations(std::vector<RhiAllocationInfo>& out) const -> void = 0;
    // Device memory heaps with driver budget when available.
    virtual auto memoryHeaps(std::vector<RhiMemoryHeapInfo>& out) const -> void = 0;
    // What a barrier between two states becomes in the backend (stages, accesses, layouts).
    [[nodiscard]] virtual auto describeTransition(RhiTextureState oldState, RhiTextureState newState) const -> RhiTransitionInfo = 0;
    [[nodiscard]] virtual auto describeTransition(RhiBufferState oldState, RhiBufferState newState) const -> RhiTransitionInfo = 0;
    // The descriptors last written to a set, by binding and array element.
    [[nodiscard]] virtual auto describeDescriptorSet(const RhiDescriptorSet* set) const -> std::vector<RhiDescriptorInfo> = 0;

    virtual auto destroyBuffer(RhiBuffer* buffer) -> void = 0;
    virtual auto destroyTexture(RhiTexture* texture) -> void = 0;
    virtual auto destroySampler(RhiSampler* sampler) -> void = 0;
    virtual auto destroyShaderModule(RhiShaderModule* module) -> void = 0;
    virtual auto destroyPipeline(RhiPipeline* pipeline) -> void = 0;
    virtual auto destroyDescriptorSetLayout(RhiDescriptorSetLayout* layout) -> void = 0;
    virtual auto destroyDescriptorPool(RhiDescriptorPool* pool) -> void = 0;
    virtual auto destroySemaphore(RhiSemaphore* semaphore) -> void = 0;
    virtual auto destroyFence(RhiFence* fence) -> void = 0;
    virtual auto destroyCommandBuffer(RhiCommandBuffer* cmd) -> void = 0;
};
