#pragma once

#include "rhitypes.h"

#include <cstdint>
#include <utility>

enum class FgAccessFlags : uint32_t {
    None = 0,
    ColorAttachment = 1 << 0,
    DepthAttachment = 1 << 1,
    ShaderRead = 1 << 2,
    TransferSrc = 1 << 3,
    TransferDst = 1 << 4,
    Present = 1 << 5,
    StorageRead = 1 << 6,  // compute or fragment reads through a storage image (General state)
    StorageWrite = 1 << 7, // compute writes through a storage image (General state)
    IndirectRead = 1 << 8, // buffer read as indirect draw arguments or counts
};

inline auto operator|(FgAccessFlags a, FgAccessFlags b) -> FgAccessFlags {
    return (FgAccessFlags) (std::to_underlying(a) | std::to_underlying(b));
}

inline auto operator&(FgAccessFlags a, FgAccessFlags b) -> bool {
    return (std::to_underlying(a) & std::to_underlying(b)) != 0;
}

struct FgTextureHandle {
    uint32_t index = UINT32_MAX;
    auto valid() const -> bool { return index != UINT32_MAX; }
};

struct FgBufferHandle {
    uint32_t index = UINT32_MAX;
    auto valid() const -> bool { return index != UINT32_MAX; }
};

struct FgTextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    RhiFormat format = RhiFormat::Undefined;
    RhiTextureUsageFlags usage = RhiTextureUsage::Sampled;
};

struct FgBufferDesc {
    uint64_t size = 0;
    RhiBufferUsageFlags usage = {};
};

enum class FgResourceKind : uint8_t {
    Texture,
    Buffer,
};

struct FgResource {
    const char* name = "";
    FgResourceKind kind = FgResourceKind::Texture;
    FgTextureDesc desc;      // textures
    FgBufferDesc bufferDesc; // buffers
    // Imported buffers: the access carried in from the previous frame, updated to the final
    // access after execute (FrameGraph::finalAccess). None for everything else.
    FgAccessFlags currentAccess = FgAccessFlags::None;
    RhiTexture* physical = nullptr;
    RhiBuffer* physicalBuffer = nullptr;
    bool external = false;
    uint32_t firstUseOrder = UINT32_MAX;
    uint32_t lastUseOrder = 0;
};
