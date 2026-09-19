#pragma once

#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "rhitypes.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

// Staging uploads for examples. Header-only. GPU-only memory cannot be mapped,
// so data goes through a host-visible staging buffer and a copy command. The
// RHI records the copies; this batch owns the command buffer, the submit and
// the wait, same as any integrator would (compare GpuUploader in the engine).
//
//   UploadBatch upload(device);
//   auto* vb = upload.buffer(bytes, RhiBufferUsage::Vertex);
//   auto* tex = upload.texture(desc, pixels);   // ends in ShaderReadOnly
//   upload.finish();                            // submit, wait, free staging
class UploadBatch {
public:
    explicit UploadBatch(RhiDevice& device) : device(device) {
        cmd = device.createCommandBuffer();
        fence = device.createFence(false);
        cmd->begin();
    }

    ~UploadBatch() {
        if (!finished) {
            finish();
        }
        device.destroyFence(fence);
        device.destroyCommandBuffer(cmd);
    }

    UploadBatch(const UploadBatch&) = delete;
    UploadBatch& operator=(const UploadBatch&) = delete;

    auto buffer(std::span<const std::byte> bytes, RhiBufferUsage usage) -> RhiBuffer* {
        auto* src = stagingFor(bytes);
        RhiBufferDesc desc = {
            .size = bytes.size(),
            .usage = usage | RhiBufferUsage::TransferDst,
            .memory = RhiMemoryUsage::GpuOnly,
        };
        auto* dst = device.createBuffer(desc);
        cmd->copyBuffer(src, dst, {.size = bytes.size()});
        return dst;
    }

    // Tightly packed pixels for mip 0, layer 0. Texture is left in ShaderReadOnly.
    auto texture(const RhiTextureDesc& desc, std::span<const std::byte> pixels) -> RhiTexture* {
        std::array<Subresource, 1> level = {{
            {.mipLevel = 0, .arrayLayer = 0, .width = desc.width, .height = desc.height, .pixels = pixels},
        }};
        return textureSubresources(desc, level);
    }

    struct Subresource {
        uint32_t mipLevel = 0;
        uint32_t arrayLayer = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        std::span<const std::byte> pixels;
    };

    // One copy per listed mip/layer. Barriers cover the whole texture, so
    // unlisted subresources end in ShaderReadOnly too, just uninitialised.
    auto textureSubresources(const RhiTextureDesc& desc, std::span<const Subresource> levels) -> RhiTexture* {
        auto texDesc = desc;
        texDesc.usage |= RhiTextureUsage::TransferDst;
        auto* dst = device.createTexture(texDesc);

        std::array<RhiTextureBarrierDesc, 1> toTransfer = {{
            {.texture = dst, .oldState = RhiTextureState::Undefined, .newState = RhiTextureState::TransferDst},
        }};
        cmd->pipelineBarrier(toTransfer);
        for (const auto& level : levels) {
            auto* src = stagingFor(level.pixels);
            cmd->copyBufferToTexture(src, dst, {.width = level.width, .height = level.height, .mipLevel = level.mipLevel, .arrayLayer = level.arrayLayer});
        }
        std::array<RhiTextureBarrierDesc, 1> toShader = {{
            {.texture = dst, .oldState = RhiTextureState::TransferDst, .newState = RhiTextureState::ShaderReadOnly},
        }};
        cmd->pipelineBarrier(toShader);
        return dst;
    }

    auto finish() -> void {
        cmd->end();
        device.submitCommandBuffer(cmd, {.fence = fence});
        device.waitForFence(fence);
        for (auto* buffer : staging) {
            device.destroyBuffer(buffer);
        }
        staging.clear();
        finished = true;
    }

private:
    auto stagingFor(std::span<const std::byte> bytes) -> RhiBuffer* {
        RhiBufferDesc desc = {
            .size = bytes.size(),
            .usage = RhiBufferUsage::TransferSrc,
            .memory = RhiMemoryUsage::CpuToGpu,
        };
        auto* buffer = device.createBuffer(desc);
        auto* mapped = device.mapBuffer(buffer);
        memcpy(mapped, bytes.data(), bytes.size());
        device.unmapBuffer(buffer);
        staging.push_back(buffer);
        return buffer;
    }

    RhiDevice& device;
    RhiCommandBuffer* cmd = nullptr;
    RhiFence* fence = nullptr;
    std::vector<RhiBuffer*> staging;
    bool finished = false;
};
