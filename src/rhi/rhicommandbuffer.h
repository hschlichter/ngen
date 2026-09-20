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
    virtual auto pipelineBarrier(std::span<const RhiTextureBarrierDesc> imageBarriers, std::span<const RhiBufferBarrierDesc> bufferBarriers) -> void = 0;
    auto pipelineBarrier(std::span<const RhiTextureBarrierDesc> imageBarriers) -> void { pipelineBarrier(imageBarriers, {}); }
    auto bufferBarrier(std::span<const RhiBufferBarrierDesc> bufferBarriers) -> void { pipelineBarrier({}, bufferBarriers); }
    // src must be in TransferSrc, dst in TransferDst. Scales between the two regions with the filter.
    virtual auto blitTexture(RhiTexture* src, RhiTexture* dst, const RhiBlitRegion& srcRegion, const RhiBlitRegion& dstRegion, RhiFilter filter) -> void = 0;
    // Mip 0 to mip 0, linear filter.
    auto blitTexture(RhiTexture* src, RhiTexture* dst, RhiExtent2D srcExtent, RhiExtent2D dstExtent) -> void {
        blitTexture(src, dst, {.mipLevel = 0, .extent = srcExtent}, {.mipLevel = 0, .extent = dstExtent}, RhiFilter::Linear);
    }
    virtual auto copyBuffer(RhiBuffer* src, RhiBuffer* dst, const RhiBufferCopy& region) -> void = 0;
    // dst must be in RhiTextureState::TransferDst.
    virtual auto copyBufferToTexture(RhiBuffer* src, RhiTexture* dst, const RhiBufferTextureCopy& region) -> void = 0;
    // src must be in RhiTextureState::TransferSrc. Readback path: dst is normally host-visible.
    virtual auto copyTextureToBuffer(RhiTexture* src, RhiBuffer* dst, const RhiBufferTextureCopy& region) -> void = 0;
    // Timestamps: reset outside beginRendering/endRendering; write anywhere. Tick period in RhiDeviceLimits.
    virtual auto resetQueryPool(RhiQueryPool* pool, uint32_t first, uint32_t count) -> void = 0;
    virtual auto writeTimestamp(RhiQueryPool* pool, uint32_t index) -> void = 0;
    // Counters since begin(); see RhiCommandStats.
    [[nodiscard]] virtual auto stats() const -> const RhiCommandStats& = 0;
    // GPU timing zones, nestable. Backend owns the queries; results via RhiDevice::collectGpuZones
    // once the submit's fence has passed. `name` must outlive execution (literals do). No-ops when
    // the device has no timestamp support or the per-command-buffer zone budget is exhausted.
    virtual auto beginGpuZone(const char* name) -> void = 0;
    virtual auto endGpuZone() -> void = 0;
    // Debug-only markers for tools like RenderDoc; no-ops when the backend has no debug extension.
    virtual auto beginLabel(const char* name) -> void = 0;
    virtual auto endLabel() -> void = 0;
    virtual auto setViewport(int32_t x, int32_t y, RhiExtent2D extent) -> void = 0;
    virtual auto setScissor(int32_t x, int32_t y, RhiExtent2D extent) -> void = 0;
    auto setViewport(RhiExtent2D extent) -> void { setViewport(0, 0, extent); }
    auto setScissor(RhiExtent2D extent) -> void { setScissor(0, 0, extent); }
    virtual auto bindPipeline(RhiPipeline* pipeline) -> void = 0;
    virtual auto bindVertexBuffer(uint32_t slot, RhiBuffer* buffer, uint64_t offset) -> void = 0;
    virtual auto bindIndexBuffer(RhiBuffer* buffer, RhiIndexType indexType, uint64_t offset) -> void = 0;
    auto bindVertexBuffer(RhiBuffer* buffer) -> void { bindVertexBuffer(0, buffer, 0); }
    auto bindIndexBuffer(RhiBuffer* buffer, RhiIndexType indexType) -> void { bindIndexBuffer(buffer, indexType, 0); }
    virtual auto bindDescriptorSet(RhiPipeline* pipeline, uint32_t setIndex, RhiDescriptorSet* set) -> void = 0;
    virtual auto pushConstants(RhiPipeline* pipeline, RhiShaderStageFlags stage, uint32_t offset, uint32_t size, const void* data) -> void = 0;
    virtual auto draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) -> void = 0;
    virtual auto drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) -> void = 0;
    // Compute; a compute pipeline must be bound. Not allowed inside beginRendering/endRendering.
    virtual auto dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) -> void = 0;
};
