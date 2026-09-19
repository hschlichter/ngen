#include "gpuuploader.h"

#include "rhicommandbuffer.h"
#include "rhidevice.h"

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

    std::array<RhiBarrierDesc, 1> toTransfer = {{
        {.texture = dst, .oldLayout = RhiImageLayout::Undefined, .newLayout = RhiImageLayout::TransferDst},
    }};
    cmd->pipelineBarrier(toTransfer);

    cmd->copyBufferToTexture(src, dst, {.width = desc.width, .height = desc.height});

    std::array<RhiBarrierDesc, 1> toShader = {{
        {.texture = dst, .oldLayout = RhiImageLayout::TransferDst, .newLayout = RhiImageLayout::ShaderReadOnly},
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
