#pragma once

#include "rhicommandbuffer.h"
#include "rhiresourcesvulkan.h"

#include <vulkan/vulkan.h>

class RhiCommandBufferVulkan : public RhiCommandBuffer {
public:
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    // Null when VK_EXT_debug_utils is not enabled; labels become no-ops.
    PFN_vkCmdBeginDebugUtilsLabelEXT beginLabelFn = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT endLabelFn = nullptr;

    auto begin() -> void override;
    auto end() -> void override;
    auto reset() -> void override;
    auto beginRendering(const RhiRenderingInfo& info) -> void override;
    auto endRendering() -> void override;
    auto pipelineBarrier(std::span<const RhiBarrierDesc> barriers) -> void override;
    auto blitTexture(RhiTexture* src, RhiTexture* dst, RhiExtent2D srcExtent, RhiExtent2D dstExtent) -> void override;
    auto copyBuffer(RhiBuffer* src, RhiBuffer* dst, const RhiBufferCopy& region) -> void override;
    auto copyBufferToTexture(RhiBuffer* src, RhiTexture* dst, const RhiBufferTextureCopy& region) -> void override;
    auto beginLabel(const char* name) -> void override;
    auto endLabel() -> void override;
    auto setViewport(int32_t x, int32_t y, RhiExtent2D extent) -> void override;
    auto setScissor(int32_t x, int32_t y, RhiExtent2D extent) -> void override;
    auto bindPipeline(RhiPipeline* pipeline) -> void override;
    auto bindVertexBuffer(RhiBuffer* buffer) -> void override;
    auto bindIndexBuffer(RhiBuffer* buffer) -> void override;
    auto bindDescriptorSet(RhiPipeline* pipeline, uint32_t setIndex, RhiDescriptorSet* set) -> void override;
    auto pushConstants(RhiPipeline* pipeline, RhiShaderStageFlags stage, uint32_t offset, uint32_t size, const void* data) -> void override;
    auto draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) -> void override;
    auto drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) -> void override;

private:
    static auto toVkImageLayout(RhiImageLayout layout) -> VkImageLayout;
    static auto layoutToAccessMask(RhiImageLayout layout) -> VkAccessFlags2;
    static auto layoutToStageMask(RhiImageLayout layout) -> VkPipelineStageFlags2;
    static auto layoutToAspectMask(RhiImageLayout layout) -> VkImageAspectFlags;
};
