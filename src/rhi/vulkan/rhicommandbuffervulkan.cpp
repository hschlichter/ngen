#include "rhicommandbuffervulkan.h"

#include <array>
#include <utility>
#include <vector>

auto RhiCommandBufferVulkan::toVkImageLayout(RhiImageLayout layout) -> VkImageLayout {
    switch (layout) {
        case RhiImageLayout::Undefined:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        case RhiImageLayout::ColorAttachment:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case RhiImageLayout::DepthStencilAttachment:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case RhiImageLayout::ShaderReadOnly:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case RhiImageLayout::TransferSrc:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case RhiImageLayout::TransferDst:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case RhiImageLayout::PresentSrc:
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

auto RhiCommandBufferVulkan::layoutToAccessMask(RhiImageLayout layout) -> VkAccessFlags2 {
    switch (layout) {
        case RhiImageLayout::Undefined:
            return VK_ACCESS_2_NONE;
        case RhiImageLayout::ColorAttachment:
            return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        case RhiImageLayout::DepthStencilAttachment:
            return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        case RhiImageLayout::ShaderReadOnly:
            return VK_ACCESS_2_SHADER_READ_BIT;
        case RhiImageLayout::TransferSrc:
            return VK_ACCESS_2_TRANSFER_READ_BIT;
        case RhiImageLayout::TransferDst:
            return VK_ACCESS_2_TRANSFER_WRITE_BIT;
        case RhiImageLayout::PresentSrc:
            return VK_ACCESS_2_NONE;
    }
    return VK_ACCESS_2_NONE;
}

auto RhiCommandBufferVulkan::layoutToStageMask(RhiImageLayout layout) -> VkPipelineStageFlags2 {
    switch (layout) {
        case RhiImageLayout::Undefined:
            return VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        case RhiImageLayout::ColorAttachment:
            return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        case RhiImageLayout::DepthStencilAttachment:
            return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        case RhiImageLayout::ShaderReadOnly:
            return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        case RhiImageLayout::TransferSrc:
            return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        case RhiImageLayout::TransferDst:
            return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        case RhiImageLayout::PresentSrc:
            return VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    }
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

auto RhiCommandBufferVulkan::layoutToAspectMask(RhiImageLayout layout) -> VkImageAspectFlags {
    if (layout == RhiImageLayout::DepthStencilAttachment) {
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

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

auto RhiCommandBufferVulkan::beginRendering(const RhiRenderingInfo& info) -> void {
    std::vector<VkRenderingAttachmentInfo> vkColorAttachments;
    vkColorAttachments.reserve(info.colorAttachments.size());

    for (const auto& att : info.colorAttachments) {
        auto* tex = static_cast<RhiTextureVulkan*>(att.texture);
        VkRenderingAttachmentInfo vkAtt = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = tex ? tex->view : VK_NULL_HANDLE,
            .imageLayout = toVkImageLayout(att.layout),
            .loadOp = att.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
        if (att.clear) {
            vkAtt.clearValue.color = {{att.clearColor[0], att.clearColor[1], att.clearColor[2], att.clearColor[3]}};
        }
        vkColorAttachments.push_back(vkAtt);
    }

    VkRenderingAttachmentInfo vkDepthAttachment = {};
    if (info.depthAttachment != nullptr) {
        auto* depthTex = static_cast<RhiTextureVulkan*>(info.depthAttachment->texture);
        vkDepthAttachment = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = depthTex ? depthTex->view : VK_NULL_HANDLE,
            .imageLayout = toVkImageLayout(info.depthAttachment->layout),
            .loadOp = info.depthAttachment->clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
        if (info.depthAttachment->clear) {
            vkDepthAttachment.clearValue.depthStencil = {.depth = info.depthAttachment->clearDepth, .stencil = 0};
        }
    }

    VkRenderingInfo vkInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, {info.extent.width, info.extent.height}},
        .layerCount = 1,
        .colorAttachmentCount = (uint32_t) vkColorAttachments.size(),
        .pColorAttachments = vkColorAttachments.data(),
        .pDepthAttachment = info.depthAttachment != nullptr ? &vkDepthAttachment : nullptr,
    };

    vkCmdBeginRendering(cmd, &vkInfo);
}

auto RhiCommandBufferVulkan::endRendering() -> void {
    vkCmdEndRendering(cmd);
}

auto RhiCommandBufferVulkan::pipelineBarrier(std::span<const RhiBarrierDesc> barriers) -> void {
    std::vector<VkImageMemoryBarrier2> imageBarriers;
    imageBarriers.reserve(barriers.size());

    for (const auto& b : barriers) {
        auto* tex = static_cast<RhiTextureVulkan*>(b.texture);
        VkImageAspectFlags aspect = layoutToAspectMask(b.newLayout);
        if (b.oldLayout == RhiImageLayout::DepthStencilAttachment) {
            aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        }

        VkImageMemoryBarrier2 barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = layoutToStageMask(b.oldLayout),
            .srcAccessMask = layoutToAccessMask(b.oldLayout),
            .dstStageMask = layoutToStageMask(b.newLayout),
            .dstAccessMask = layoutToAccessMask(b.newLayout),
            .oldLayout = toVkImageLayout(b.oldLayout),
            .newLayout = toVkImageLayout(b.newLayout),
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = tex->image,
            .subresourceRange =
                {
                    .aspectMask = aspect,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        };
        imageBarriers.push_back(barrier);
    }

    VkDependencyInfo depInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = (uint32_t) imageBarriers.size(),
        .pImageMemoryBarriers = imageBarriers.data(),
    };

    vkCmdPipelineBarrier2(cmd, &depInfo);
}

auto RhiCommandBufferVulkan::setViewport(int32_t x, int32_t y, RhiExtent2D extent) -> void {
    VkViewport viewport = {(float) x, (float) y, (float) extent.width, (float) extent.height, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
}

auto RhiCommandBufferVulkan::setScissor(int32_t x, int32_t y, RhiExtent2D extent) -> void {
    VkRect2D scissor = {{x, y}, {extent.width, extent.height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
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

auto RhiCommandBufferVulkan::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) -> void {
    vkCmdDraw(cmd, vertexCount, instanceCount, firstVertex, firstInstance);
}

auto RhiCommandBufferVulkan::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
    -> void {
    vkCmdDrawIndexed(cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}
