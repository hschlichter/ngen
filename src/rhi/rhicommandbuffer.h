#pragma once

#include "rhitypes.h"

#include <span>

class RhiCommandBuffer {
public:
    RhiCommandBuffer() = default;
    RhiCommandBuffer(const RhiCommandBuffer&) = delete;
    RhiCommandBuffer& operator=(const RhiCommandBuffer&) = delete;
    RhiCommandBuffer(RhiCommandBuffer&&) = default;
    RhiCommandBuffer& operator=(RhiCommandBuffer&&) = default;
    virtual ~RhiCommandBuffer() = default;

    virtual auto begin() -> void = 0;
    virtual auto end() -> void = 0;
    virtual auto reset() -> void = 0;
    virtual auto beginRendering(const RhiRenderingInfo& info) -> void = 0;
    virtual auto endRendering() -> void = 0;
    virtual auto pipelineBarrier(std::span<const RhiBarrierDesc> barriers) -> void = 0;
    virtual auto bindPipeline(RhiPipeline* pipeline) -> void = 0;
    virtual auto bindVertexBuffer(RhiBuffer* buffer) -> void = 0;
    virtual auto bindIndexBuffer(RhiBuffer* buffer) -> void = 0;
    virtual auto bindDescriptorSet(RhiPipeline* pipeline, RhiDescriptorSet* set) -> void = 0;
    virtual auto pushConstants(RhiPipeline* pipeline, RhiShaderStage stage, uint32_t offset, uint32_t size, const void* data) -> void = 0;
    virtual auto drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) -> void = 0;
};
