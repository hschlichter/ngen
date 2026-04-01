#include "rhicommandbuffervulkan.h"

void RhiCommandBufferVulkan::begin() {
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    vkBeginCommandBuffer(cmd, &beginInfo);
}

void RhiCommandBufferVulkan::end() {
    vkEndCommandBuffer(cmd);
}

void RhiCommandBufferVulkan::reset() {
    vkResetCommandBuffer(cmd, 0);
}

void RhiCommandBufferVulkan::beginRenderPass(const RhiRenderPassBeginDesc& desc) {
    auto* rp = static_cast<RhiRenderPassVulkan*>(desc.renderPass);
    auto* fb = static_cast<RhiFramebufferVulkan*>(desc.framebuffer);

    VkClearValue clearValues[2] = {};
    clearValues[0].color = {{desc.clearColor[0], desc.clearColor[1], desc.clearColor[2], desc.clearColor[3]}};
    clearValues[1].depthStencil = {desc.clearDepth, 0};

    VkRenderPassBeginInfo passBegin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rp->renderPass,
        .framebuffer = fb->framebuffer,
        .renderArea = {{0, 0}, {desc.extent.width, desc.extent.height}},
        .clearValueCount = 2,
        .pClearValues = clearValues,
    };

    vkCmdBeginRenderPass(cmd, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
}

void RhiCommandBufferVulkan::endRenderPass() {
    vkCmdEndRenderPass(cmd);
}

void RhiCommandBufferVulkan::bindPipeline(RhiPipeline* pipeline) {
    auto* p = static_cast<RhiPipelineVulkan*>(pipeline);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
}

void RhiCommandBufferVulkan::bindVertexBuffer(RhiBuffer* buffer) {
    auto* b = static_cast<RhiBufferVulkan*>(buffer);
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &b->buffer, offsets);
}

void RhiCommandBufferVulkan::bindIndexBuffer(RhiBuffer* buffer) {
    auto* b = static_cast<RhiBufferVulkan*>(buffer);
    vkCmdBindIndexBuffer(cmd, b->buffer, 0, VK_INDEX_TYPE_UINT32);
}

void RhiCommandBufferVulkan::bindDescriptorSet(RhiPipeline* pipeline, RhiDescriptorSet* set) {
    auto* p = static_cast<RhiPipelineVulkan*>(pipeline);
    auto* s = static_cast<RhiDescriptorSetVulkan*>(set);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &s->set, 0, NULL);
}

void RhiCommandBufferVulkan::pushConstants(RhiPipeline* pipeline, RhiShaderStage stage, uint32_t offset, uint32_t size, const void* data) {
    auto* p = static_cast<RhiPipelineVulkan*>(pipeline);
    VkShaderStageFlags vkStage = 0;
    if ((uint32_t) stage & (uint32_t) RhiShaderStage::Vertex) {
        vkStage |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if ((uint32_t) stage & (uint32_t) RhiShaderStage::Fragment) {
        vkStage |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    vkCmdPushConstants(cmd, p->layout, vkStage, offset, size, data);
}

void RhiCommandBufferVulkan::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    vkCmdDrawIndexed(cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}
