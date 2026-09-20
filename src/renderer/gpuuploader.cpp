#include "gpuuploader.h"

#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <algorithm>
#include <array>
#include <cstring>

auto GpuUploader::init(RhiDevice* dev) -> void {
    device = dev;
    cmd = device->createCommandBuffer();
    fence = device->createFence(false);
}

auto GpuUploader::destroy() -> void {
    if (device == nullptr) {
        return;
    }
    device->destroyFence(fence);
    device->destroyCommandBuffer(cmd);
    fence = nullptr;
    cmd = nullptr;
    device = nullptr;
}

auto GpuUploader::begin() -> void {
    cmd->reset();
    cmd->begin();
    recording = true;
}

auto GpuUploader::createStaging(std::span<const std::byte> data) -> RhiBuffer* {
    RhiBufferDesc desc = {
        .size = data.size(),
        .usage = RhiBufferUsage::TransferSrc,
        .memory = RhiMemoryUsage::CpuToGpu,
    };
    auto* buffer = device->createBuffer(desc);
    auto* mapped = device->mapBuffer(buffer);
    memcpy(mapped, data.data(), data.size());
    device->unmapBuffer(buffer);
    staging.push_back(buffer);
    return buffer;
}

auto GpuUploader::uploadBuffer(std::span<const std::byte> data, RhiBufferUsageFlags usage) -> RhiBuffer* {
    auto* src = createStaging(data);

    RhiBufferDesc desc = {
        .size = data.size(),
        .usage = usage | RhiBufferUsage::TransferDst,
        .memory = RhiMemoryUsage::GpuOnly,
    };
    auto* dst = device->createBuffer(desc);

    cmd->copyBuffer(src, dst, {.size = data.size()});
    return dst;
}

auto GpuUploader::uploadTexture(const RhiTextureDesc& desc, std::span<const std::byte> pixels) -> RhiTexture* {
    auto* src = createStaging(pixels);

    auto texDesc = desc;
    texDesc.usage |= RhiTextureUsage::TransferDst;
    auto* dst = device->createTexture(texDesc);

    std::array<RhiTextureBarrierDesc, 1> toTransfer = {{
        {.texture = dst, .oldState = RhiTextureState::Undefined, .newState = RhiTextureState::TransferDst},
    }};
    cmd->pipelineBarrier(toTransfer);

    // One copy per level, walking the packed staging block. Barriers cover the whole
    // image, so the chain moves to ShaderReadOnly together.
    uint64_t offset = 0;
    uint32_t w = desc.width;
    uint32_t h = desc.height;
    for (uint32_t level = 0; level < std::max(1u, desc.mipLevels); level++) {
        cmd->copyBufferToTexture(src, dst, {.bufferOffset = offset, .width = w, .height = h, .mipLevel = level});
        offset += (uint64_t) w * h * 4;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }

    std::array<RhiTextureBarrierDesc, 1> toShader = {{
        {.texture = dst, .oldState = RhiTextureState::TransferDst, .newState = RhiTextureState::ShaderReadOnly},
    }};
    cmd->pipelineBarrier(toShader);

    return dst;
}

auto GpuUploader::end() -> void {
    cmd->end();
    recording = false;

    device->submitCommandBuffer(cmd, {.fence = fence});
    device->waitForFence(fence);
    device->resetFence(fence);

    for (auto* buffer : staging) {
        device->destroyBuffer(buffer);
    }
    staging.clear();
}
