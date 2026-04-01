#pragma once

#include "rhitypes.h"

#include <vector>

class RhiSwapchain;
class RhiCommandBuffer;
struct SDL_Window;

class RhiDevice {
public:
    virtual ~RhiDevice() = default;

    virtual int init(SDL_Window* window) = 0;
    virtual void destroy() = 0;
    virtual void waitIdle() = 0;

    virtual RhiSwapchain* createSwapchain(SDL_Window* window) = 0;
    virtual RhiBuffer* createBuffer(const RhiBufferDesc& desc) = 0;
    virtual RhiTexture* createTexture(const RhiTextureDesc& desc) = 0;
    virtual RhiSampler* createSampler(const RhiSamplerDesc& desc) = 0;
    virtual RhiShaderModule* createShaderModule(const char* filepath) = 0;
    virtual RhiPipeline* createGraphicsPipeline(const RhiGraphicsPipelineDesc& desc) = 0;
    virtual RhiDescriptorSetLayout* createDescriptorSetLayout(const RhiDescriptorBinding* bindings, uint32_t count) = 0;
    virtual RhiDescriptorPool* createDescriptorPool(uint32_t maxSets, const RhiDescriptorBinding* bindings, uint32_t bindingCount) = 0;
    virtual std::vector<RhiDescriptorSet*> allocateDescriptorSets(RhiDescriptorPool* pool, RhiDescriptorSetLayout* layout, uint32_t count) = 0;
    virtual void updateDescriptorSet(RhiDescriptorSet* set, const RhiDescriptorWrite* writes, uint32_t writeCount) = 0;

    virtual RhiCommandBuffer* createCommandBuffer() = 0;
    virtual RhiSemaphore* createSemaphore() = 0;
    virtual RhiFence* createFence(bool signaled) = 0;

    virtual void waitForFence(RhiFence* fence) = 0;
    virtual void resetFence(RhiFence* fence) = 0;
    virtual void submitCommandBuffer(RhiCommandBuffer* cmd, const RhiSubmitInfo& info) = 0;
    virtual void present(RhiSwapchain* swapchain, RhiSemaphore* waitSemaphore, uint32_t imageIndex) = 0;

    virtual void* mapBuffer(RhiBuffer* buffer) = 0;
    virtual void unmapBuffer(RhiBuffer* buffer) = 0;
    virtual void copyBuffer(RhiBuffer* src, RhiBuffer* dst, uint64_t size) = 0;

    virtual void destroyBuffer(RhiBuffer* buffer) = 0;
    virtual void destroyTexture(RhiTexture* texture) = 0;
    virtual void destroySampler(RhiSampler* sampler) = 0;
    virtual void destroyShaderModule(RhiShaderModule* module) = 0;
    virtual void destroyPipeline(RhiPipeline* pipeline) = 0;
    virtual void destroyDescriptorSetLayout(RhiDescriptorSetLayout* layout) = 0;
    virtual void destroyDescriptorPool(RhiDescriptorPool* pool) = 0;
    virtual void destroySemaphore(RhiSemaphore* semaphore) = 0;
    virtual void destroyFence(RhiFence* fence) = 0;
    virtual void destroyCommandBuffer(RhiCommandBuffer* cmd) = 0;
};
