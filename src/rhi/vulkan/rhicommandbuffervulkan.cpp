#include "rhicommandbuffervulkan.h"

#include <array>
#include <utility>

auto RhiCommandBufferVulkan::begin() -> void {
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    vkBeginCommandBuffer(cmd, &beginInfo);
}

auto RhiCommandBufferVulkan::end() -> void {
    vkEndCommandBuffer(cmd);
}

auto RhiCommandBufferVulkan::reset() -> void {
    vkResetCommandBuffer(cmd, 0);
}

auto RhiCommandBufferVulkan::beginRenderPass(const RhiRenderPassBeginDesc& desc) -> void {
    auto* rp = static_cast<RhiRenderPassVulkan*>(desc.renderPass);
    auto* fb = static_cast<RhiFramebufferVulkan*>(desc.framebuffer);

    std::array<VkClearValue, 2> clearValues = {};
    clearValues[0].color = {{desc.clearColor[0], desc.clearColor[1], desc.clearColor[2], desc.clearColor[3]}};
    clearValues[1].depthStencil = {.depth = desc.clearDepth, .stencil = 0};

    VkRenderPassBeginInfo passBegin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rp->renderPass,
        .framebuffer = fb->framebuffer,
        .renderArea = {{0, 0}, {desc.extent.width, desc.extent.height}},
        .clearValueCount = 2,
        .pClearValues = clearValues.data(),
    };

    vkCmdBeginRenderPass(cmd, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
}

auto RhiCommandBufferVulkan::endRenderPass() -> void {
    vkCmdEndRenderPass(cmd);
}

auto RhiCommandBufferVulkan::bindPipeline(RhiPipeline* pipeline) -> void {
    auto* p = static_cast<RhiPipelineVulkan*>(pipeline);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
}

auto RhiCommandBufferVulkan::bindVertexBuffer(RhiBuffer* buffer) -> void {
    auto* b = static_cast<RhiBufferVulkan*>(buffer);
    std::array<VkDeviceSize, 1> offsets = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &b->buffer, offsets.data());
}

auto RhiCommandBufferVulkan::bindIndexBuffer(RhiBuffer* buffer) -> void {
    auto* b = static_cast<RhiBufferVulkan*>(buffer);
    vkCmdBindIndexBuffer(cmd, b->buffer, 0, VK_INDEX_TYPE_UINT32);
}

auto RhiCommandBufferVulkan::bindDescriptorSet(RhiPipeline* pipeline, RhiDescriptorSet* set) -> void {
    auto* p = static_cast<RhiPipelineVulkan*>(pipeline);
    auto* s = static_cast<RhiDescriptorSetVulkan*>(set);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &s->set, 0, nullptr);
}

auto RhiCommandBufferVulkan::pushConstants(RhiPipeline* pipeline, RhiShaderStage stage, uint32_t offset, uint32_t size, const void* data) -> void {
    auto* p = static_cast<RhiPipelineVulkan*>(pipeline);
    VkShaderStageFlags vkStage = 0;
    if ((std::to_underlying(stage) & std::to_underlying(RhiShaderStage::Vertex)) != 0u) {
        vkStage |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if ((std::to_underlying(stage) & std::to_underlying(RhiShaderStage::Fragment)) != 0u) {
        vkStage |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    vkCmdPushConstants(cmd, p->layout, vkStage, offset, size, data);
}

auto RhiCommandBufferVulkan::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
    -> void {
    vkCmdDrawIndexed(cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}
