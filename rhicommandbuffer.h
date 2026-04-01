#pragma once

#include "rhitypes.h"

class RhiCommandBuffer {
public:
    virtual ~RhiCommandBuffer() = default;

    virtual void begin() = 0;
    virtual void end() = 0;
    virtual void reset() = 0;
    virtual void beginRenderPass(const RhiRenderPassBeginDesc& desc) = 0;
    virtual void endRenderPass() = 0;
    virtual void bindPipeline(RhiPipeline* pipeline) = 0;
    virtual void bindVertexBuffer(RhiBuffer* buffer) = 0;
    virtual void bindIndexBuffer(RhiBuffer* buffer) = 0;
    virtual void bindDescriptorSet(RhiPipeline* pipeline, RhiDescriptorSet* set) = 0;
    virtual void pushConstants(RhiPipeline* pipeline, RhiShaderStage stage, uint32_t offset, uint32_t size, const void* data) = 0;
    virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) = 0;
};
