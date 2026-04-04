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
};

struct FgResource {
    FgTextureDesc desc;
    FgAccessFlags currentAccess = FgAccessFlags::None;
    RhiTexture* physical = nullptr;
    bool external = false;
};
