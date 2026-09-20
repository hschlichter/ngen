#include "rhicommandbuffervulkan.h"

#include <array>
#include <utility>
#include <vector>

auto RhiCommandBufferVulkan::toVkImageLayout(RhiTextureState layout) -> VkImageLayout {
    switch (layout) {
        case RhiTextureState::Undefined:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        case RhiTextureState::ColorAttachment:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case RhiTextureState::DepthStencilAttachment:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case RhiTextureState::ShaderReadOnly:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case RhiTextureState::General:
            return VK_IMAGE_LAYOUT_GENERAL;
        case RhiTextureState::TransferSrc:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case RhiTextureState::TransferDst:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case RhiTextureState::PresentSrc:
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

auto RhiCommandBufferVulkan::stateToAccessMask(RhiTextureState layout) -> VkAccessFlags2 {
    switch (layout) {
        case RhiTextureState::Undefined:
            return VK_ACCESS_2_NONE;
        case RhiTextureState::ColorAttachment:
            return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        case RhiTextureState::DepthStencilAttachment:
            return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        case RhiTextureState::ShaderReadOnly:
            return VK_ACCESS_2_SHADER_READ_BIT;
        case RhiTextureState::General:
            return VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        case RhiTextureState::TransferSrc:
            return VK_ACCESS_2_TRANSFER_READ_BIT;
        case RhiTextureState::TransferDst:
            return VK_ACCESS_2_TRANSFER_WRITE_BIT;
        case RhiTextureState::PresentSrc:
            return VK_ACCESS_2_NONE;
    }
    return VK_ACCESS_2_NONE;
}

auto RhiCommandBufferVulkan::stateToStageMask(RhiTextureState layout) -> VkPipelineStageFlags2 {
    switch (layout) {
        case RhiTextureState::Undefined:
            return VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        case RhiTextureState::ColorAttachment:
            return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        case RhiTextureState::DepthStencilAttachment:
            return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        case RhiTextureState::ShaderReadOnly:
            return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case RhiTextureState::General:
            return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case RhiTextureState::TransferSrc:
            return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        case RhiTextureState::TransferDst:
            return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        case RhiTextureState::PresentSrc:
            return VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    }
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

auto RhiCommandBufferVulkan::begin() -> void {
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    vkBeginCommandBuffer(cmd, &beginInfo);

    commandStats = {};
    zones.clear();
    zoneStack.clear();
    if (zonePool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, zonePool, 0, maxGpuZones * 2);
    }
}

auto RhiCommandBufferVulkan::beginGpuZone(const char* name) -> void {
    if (zonePool == VK_NULL_HANDLE || zones.size() >= maxGpuZones) {
        zoneStack.push_back(UINT32_MAX); // keep the stack balanced for endGpuZone
        return;
    }
    auto index = (uint32_t) zones.size();
    zones.push_back({.name = name, .depth = (uint16_t) zoneStack.size(), .closed = false});
    zoneStack.push_back(index);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, zonePool, index * 2);
}

auto RhiCommandBufferVulkan::endGpuZone() -> void {
    if (zoneStack.empty()) {
        return;
    }
    auto index = zoneStack.back();
    zoneStack.pop_back();
    if (index == UINT32_MAX) {
        return;
    }
    zones[index].closed = true;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, zonePool, index * 2 + 1);
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
            .imageLayout = toVkImageLayout(att.state),
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
            .imageLayout = toVkImageLayout(info.depthAttachment->state),
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

auto RhiCommandBufferVulkan::bufferStateToAccessMask(RhiBufferState state) -> VkAccessFlags2 {
    switch (state) {
        case RhiBufferState::Undefined:
            return VK_ACCESS_2_NONE;
        case RhiBufferState::VertexRead:
            return VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        case RhiBufferState::IndexRead:
            return VK_ACCESS_2_INDEX_READ_BIT;
        case RhiBufferState::UniformRead:
            return VK_ACCESS_2_UNIFORM_READ_BIT;
        case RhiBufferState::StorageRead:
            return VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        case RhiBufferState::StorageWrite:
            return VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        case RhiBufferState::TransferSrc:
            return VK_ACCESS_2_TRANSFER_READ_BIT;
        case RhiBufferState::TransferDst:
            return VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    return VK_ACCESS_2_NONE;
}

auto RhiCommandBufferVulkan::bufferStateToStageMask(RhiBufferState state) -> VkPipelineStageFlags2 {
    switch (state) {
        case RhiBufferState::Undefined:
            return VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        case RhiBufferState::VertexRead:
        case RhiBufferState::IndexRead:
            return VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        case RhiBufferState::UniformRead:
        case RhiBufferState::StorageRead:
        case RhiBufferState::StorageWrite:
            return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case RhiBufferState::TransferSrc:
        case RhiBufferState::TransferDst:
            return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    }
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

auto RhiCommandBufferVulkan::pipelineBarrier(std::span<const RhiTextureBarrierDesc> barriers, std::span<const RhiBufferBarrierDesc> bufferBarrierDescs) -> void {
    commandStats.barriers += (uint32_t) (barriers.size() + bufferBarrierDescs.size());
    std::vector<VkBufferMemoryBarrier2> bufferBarriers;
    bufferBarriers.reserve(bufferBarrierDescs.size());
    for (const auto& b : bufferBarrierDescs) {
        auto* buf = static_cast<RhiBufferVulkan*>(b.buffer);
        VkBufferMemoryBarrier2 barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = bufferStateToStageMask(b.oldState),
            .srcAccessMask = bufferStateToAccessMask(b.oldState),
            .dstStageMask = bufferStateToStageMask(b.newState),
            .dstAccessMask = bufferStateToAccessMask(b.newState),
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buf->buffer,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        bufferBarriers.push_back(barrier);
    }

    std::vector<VkImageMemoryBarrier2> imageBarriers;
    imageBarriers.reserve(barriers.size());

    for (const auto& b : barriers) {
        auto* tex = static_cast<RhiTextureVulkan*>(b.texture);

        VkImageMemoryBarrier2 barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = stateToStageMask(b.oldState),
            .srcAccessMask = stateToAccessMask(b.oldState),
            .dstStageMask = stateToStageMask(b.newState),
            .dstAccessMask = stateToAccessMask(b.newState),
            .oldLayout = toVkImageLayout(b.oldState),
            .newLayout = toVkImageLayout(b.newState),
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = tex->image,
            .subresourceRange =
                {
                    .aspectMask = tex->aspect,
                    .baseMipLevel = 0,
                    .levelCount = tex->mipLevels,
                    .baseArrayLayer = 0,
                    .layerCount = tex->arrayLayers,
                },
        };
        imageBarriers.push_back(barrier);
    }

    VkDependencyInfo depInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = (uint32_t) bufferBarriers.size(),
        .pBufferMemoryBarriers = bufferBarriers.data(),
        .imageMemoryBarrierCount = (uint32_t) imageBarriers.size(),
        .pImageMemoryBarriers = imageBarriers.data(),
    };

    vkCmdPipelineBarrier2(cmd, &depInfo);
}

auto RhiCommandBufferVulkan::blitTexture(RhiTexture* src, RhiTexture* dst, const RhiBlitRegion& srcRegion, const RhiBlitRegion& dstRegion, RhiFilter filter) -> void {
    commandStats.copies++;
    auto* srcTex = static_cast<RhiTextureVulkan*>(src);
    auto* dstTex = static_cast<RhiTextureVulkan*>(dst);
    VkImageBlit region = {
        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = srcRegion.mipLevel, .baseArrayLayer = 0, .layerCount = 1},
        .srcOffsets = {{0, 0, 0}, {(int32_t) srcRegion.extent.width, (int32_t) srcRegion.extent.height, 1}},
        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = dstRegion.mipLevel, .baseArrayLayer = 0, .layerCount = 1},
        .dstOffsets = {{0, 0, 0}, {(int32_t) dstRegion.extent.width, (int32_t) dstRegion.extent.height, 1}},
    };
    auto vkFilter = filter == RhiFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    vkCmdBlitImage(cmd, srcTex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstTex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, vkFilter);
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
    commandStats.pipelineBinds++;
    boundTopology = p->topology;
    vkCmdBindPipeline(cmd, p->bindPoint, p->pipeline);
}

auto RhiCommandBufferVulkan::bindVertexBuffer(uint32_t slot, RhiBuffer* buffer, uint64_t offset) -> void {
    auto* b = static_cast<RhiBufferVulkan*>(buffer);
    VkDeviceSize vkOffset = offset;
    vkCmdBindVertexBuffers(cmd, slot, 1, &b->buffer, &vkOffset);
}

auto RhiCommandBufferVulkan::bindIndexBuffer(RhiBuffer* buffer, RhiIndexType indexType, uint64_t offset) -> void {
    auto* b = static_cast<RhiBufferVulkan*>(buffer);
    auto vkType = indexType == RhiIndexType::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer(cmd, b->buffer, offset, vkType);
}

auto RhiCommandBufferVulkan::bindDescriptorSet(RhiPipeline* pipeline, uint32_t setIndex, RhiDescriptorSet* set) -> void {
    commandStats.descriptorBinds++;
    auto* p = static_cast<RhiPipelineVulkan*>(pipeline);
    auto* s = static_cast<RhiDescriptorSetVulkan*>(set);
    vkCmdBindDescriptorSets(cmd, p->bindPoint, p->layout, setIndex, 1, &s->set, 0, nullptr);
}

auto RhiCommandBufferVulkan::pushConstants(RhiPipeline* pipeline, RhiShaderStageFlags stage, uint32_t offset, uint32_t size, const void* data) -> void {
    auto* p = static_cast<RhiPipelineVulkan*>(pipeline);
    VkShaderStageFlags vkStage = 0;
    if (stage.has(RhiShaderStage::Vertex)) {
        vkStage |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (stage.has(RhiShaderStage::Fragment)) {
        vkStage |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    if (stage.has(RhiShaderStage::Compute)) {
        vkStage |= VK_SHADER_STAGE_COMPUTE_BIT;
    }
    vkCmdPushConstants(cmd, p->layout, vkStage, offset, size, data);
}

auto RhiCommandBufferVulkan::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) -> void {
    commandStats.draws++;
    commandStats.primitives += (uint64_t) (boundTopology == RhiPrimitiveTopology::LineList ? vertexCount / 2 : vertexCount / 3) * instanceCount;
    vkCmdDraw(cmd, vertexCount, instanceCount, firstVertex, firstInstance);
}

auto RhiCommandBufferVulkan::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
    -> void {
    commandStats.draws++;
    commandStats.primitives += (uint64_t) (boundTopology == RhiPrimitiveTopology::LineList ? indexCount / 2 : indexCount / 3) * instanceCount;
    vkCmdDrawIndexed(cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

auto RhiCommandBufferVulkan::copyBuffer(RhiBuffer* src, RhiBuffer* dst, const RhiBufferCopy& region) -> void {
    commandStats.copies++;
    auto* srcBuf = static_cast<RhiBufferVulkan*>(src);
    auto* dstBuf = static_cast<RhiBufferVulkan*>(dst);
    VkBufferCopy vkRegion = {
        .srcOffset = region.srcOffset,
        .dstOffset = region.dstOffset,
        .size = region.size,
    };
    vkCmdCopyBuffer(cmd, srcBuf->buffer, dstBuf->buffer, 1, &vkRegion);
}

auto RhiCommandBufferVulkan::copyBufferToTexture(RhiBuffer* src, RhiTexture* dst, const RhiBufferTextureCopy& region) -> void {
    commandStats.copies++;
    auto* srcBuf = static_cast<RhiBufferVulkan*>(src);
    auto* dstTex = static_cast<RhiTextureVulkan*>(dst);
    VkBufferImageCopy vkRegion = {
        .bufferOffset = region.bufferOffset,
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = region.mipLevel, .baseArrayLayer = region.arrayLayer, .layerCount = 1},
        .imageExtent = {region.width, region.height, 1},
    };
    vkCmdCopyBufferToImage(cmd, srcBuf->buffer, dstTex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &vkRegion);
}

auto RhiCommandBufferVulkan::copyTextureToBuffer(RhiTexture* src, RhiBuffer* dst, const RhiBufferTextureCopy& region) -> void {
    commandStats.copies++;
    auto* srcTex = static_cast<RhiTextureVulkan*>(src);
    auto* dstBuf = static_cast<RhiBufferVulkan*>(dst);
    VkBufferImageCopy vkRegion = {
        .bufferOffset = region.bufferOffset,
        .imageSubresource = {.aspectMask = srcTex->aspect, .mipLevel = region.mipLevel, .baseArrayLayer = region.arrayLayer, .layerCount = 1},
        .imageExtent = {region.width, region.height, 1},
    };
    vkCmdCopyImageToBuffer(cmd, srcTex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstBuf->buffer, 1, &vkRegion);
}

auto RhiCommandBufferVulkan::resetQueryPool(RhiQueryPool* pool, uint32_t first, uint32_t count) -> void {
    auto* p = static_cast<RhiQueryPoolVulkan*>(pool);
    vkCmdResetQueryPool(cmd, p->pool, first, count);
}

auto RhiCommandBufferVulkan::writeTimestamp(RhiQueryPool* pool, uint32_t index) -> void {
    auto* p = static_cast<RhiQueryPoolVulkan*>(pool);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, p->pool, index);
}

auto RhiCommandBufferVulkan::beginLabel(const char* name) -> void {
    if (beginLabelFn == nullptr) {
        return;
    }
    VkDebugUtilsLabelEXT label = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        .pLabelName = name,
        .color = {1.0f, 1.0f, 1.0f, 1.0f},
    };
    beginLabelFn(cmd, &label);
}

auto RhiCommandBufferVulkan::endLabel() -> void {
    if (endLabelFn == nullptr) {
        return;
    }
    endLabelFn(cmd);
}

auto RhiCommandBufferVulkan::dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) -> void {
    commandStats.dispatches++;
    vkCmdDispatch(cmd, groupsX, groupsY, groupsZ);
}
