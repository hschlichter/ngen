#pragma once

#include "rhitypes.h"

#include <expected>
#include <span>
#include <vector>

class RhiSwapchain;
class RhiCommandBuffer;
struct SDL_Window;

class RhiDevice {
public:
    RhiDevice() = default;
    RhiDevice(const RhiDevice&) = delete;
    RhiDevice& operator=(const RhiDevice&) = delete;
    RhiDevice(RhiDevice&&) = default;
    RhiDevice& operator=(RhiDevice&&) = default;
    virtual ~RhiDevice() = default;

    virtual auto init(SDL_Window* window) -> std::expected<void, int> = 0;
    virtual auto destroy() -> void = 0;
    virtual auto waitIdle() -> void = 0;

    virtual auto createSwapchain(SDL_Window* window) -> RhiSwapchain* = 0;
    virtual auto createBuffer(const RhiBufferDesc& desc) -> RhiBuffer* = 0;
    virtual auto createTexture(const RhiTextureDesc& desc) -> RhiTexture* = 0;
    virtual auto createSampler(const RhiSamplerDesc& desc) -> RhiSampler* = 0;
    virtual auto createShaderModule(const char* filepath) -> RhiShaderModule* = 0;
    virtual auto createGraphicsPipeline(const RhiGraphicsPipelineDesc& desc) -> RhiPipeline* = 0;
    virtual auto createDescriptorSetLayout(std::span<const RhiDescriptorBinding> bindings) -> RhiDescriptorSetLayout* = 0;
    virtual auto createDescriptorPool(uint32_t maxSets, std::span<const RhiDescriptorBinding> bindings) -> RhiDescriptorPool* = 0;
    virtual auto allocateDescriptorSets(RhiDescriptorPool* pool, RhiDescriptorSetLayout* layout, uint32_t count) -> std::vector<RhiDescriptorSet*> = 0;
    virtual auto updateDescriptorSet(RhiDescriptorSet* set, std::span<const RhiDescriptorWrite> writes) -> void = 0;

    virtual auto createCommandBuffer() -> RhiCommandBuffer* = 0;
    virtual auto createSemaphore() -> RhiSemaphore* = 0;
    virtual auto createFence(bool signaled) -> RhiFence* = 0;

    virtual auto waitForFence(RhiFence* fence) -> void = 0;
    virtual auto resetFence(RhiFence* fence) -> void = 0;
    virtual auto submitCommandBuffer(RhiCommandBuffer* cmd, const RhiSubmitInfo& info) -> void = 0;
    virtual auto present(RhiSwapchain* swapchain, RhiSemaphore* waitSemaphore, uint32_t imageIndex) -> bool = 0;

    virtual auto mapBuffer(RhiBuffer* buffer) -> void* = 0;
    virtual auto unmapBuffer(RhiBuffer* buffer) -> void = 0;
    virtual auto copyBuffer(RhiBuffer* src, RhiBuffer* dst, uint64_t size) -> void = 0;

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
