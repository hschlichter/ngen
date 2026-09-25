#pragma once

#include "rhitypes.h"

#include <cstddef>
#include <span>
#include <vector>

class RhiDevice;
class RhiCommandBuffer;

// Batches CPU-to-GPU uploads into one command buffer and one fence wait.
//
//   uploader.begin();
//   auto* vb = uploader.uploadBuffer(bytes, RhiBufferUsage::Vertex, "mesh.vertices");
//   auto* tex = uploader.uploadTexture(desc, pixels);
//   uploader.end(); // submits, waits, frees staging
//
// Textures come back in RhiTextureState::ShaderReadOnly. Call end() before using
// anything returned by upload*(). Single-threaded; owned by the renderer.
class GpuUploader {
public:
    auto init(RhiDevice* device) -> void;
    auto destroy() -> void;

    auto begin() -> void;
    // name: debug name of the new buffer (see RhiDevice::setDebugName).
    auto uploadBuffer(std::span<const std::byte> data, RhiBufferUsageFlags usage, const char* name) -> RhiBuffer*;
    // pixels holds every mip level tightly packed in level order when desc.mipLevels > 1
    // (see packMipChain); 4 bytes per texel, so RGBA8 formats only.
    auto uploadTexture(const RhiTextureDesc& desc, std::span<const std::byte> pixels) -> RhiTexture*;
    auto end() -> void;

private:
    auto createStaging(std::span<const std::byte> data) -> RhiBuffer*;

    RhiDevice* device = nullptr;
    RhiCommandBuffer* cmd = nullptr;
    RhiFence* fence = nullptr;
    std::vector<RhiBuffer*> staging;
    bool recording = false;
};
