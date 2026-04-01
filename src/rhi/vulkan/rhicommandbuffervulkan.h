#pragma once

#include "rhicommandbuffer.h"
#include "rhiresourcesvulkan.h"

#include <vulkan/vulkan.h>

class RhiCommandBufferVulkan : public RhiCommandBuffer {
public:
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    auto begin() -> void override;
    auto end() -> void override;
    auto reset() -> void override;
    auto beginRenderPass(const RhiRenderPassBeginDesc& desc) -> void override;
    auto endRenderPass() -> void override;
    auto bindPipeline(RhiPipeline* pipeline) -> void override;
    auto bindVertexBuffer(RhiBuffer* buffer) -> void override;
    auto bindIndexBuffer(RhiBuffer* buffer) -> void override;
    auto bindDescriptorSet(RhiPipeline* pipeline, RhiDescriptorSet* set) -> void override;
    auto pushConstants(RhiPipeline* pipeline, RhiShaderStage stage, uint32_t offset, uint32_t size, const void* data) -> void override;
    auto drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) -> void override;
};
