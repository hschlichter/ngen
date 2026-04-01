#pragma once

#include "rhicommandbuffer.h"
#include "rhiresourcesvulkan.h"

#include <vulkan/vulkan.h>

class RhiCommandBufferVulkan : public RhiCommandBuffer {
public:
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    void begin() override;
    void end() override;
    void reset() override;
    void beginRenderPass(const RhiRenderPassBeginDesc& desc) override;
    void endRenderPass() override;
    void bindPipeline(RhiPipeline* pipeline) override;
    void bindVertexBuffer(RhiBuffer* buffer) override;
    void bindIndexBuffer(RhiBuffer* buffer) override;
    void bindDescriptorSet(RhiPipeline* pipeline, RhiDescriptorSet* set) override;
    void pushConstants(RhiPipeline* pipeline, RhiShaderStage stage,
                       uint32_t offset, uint32_t size, const void* data) override;
    void drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                     uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
};
