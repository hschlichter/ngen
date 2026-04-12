#pragma once

#include "rhitypes.h"

#include <cstdint>
#include <vector>

class RhiDevice;

struct ResourcePoolKey {
    uint32_t width;
    uint32_t height;
    RhiFormat format;
    RhiTextureUsage usage;

    auto operator==(const ResourcePoolKey& other) const -> bool {
        return width == other.width && height == other.height && format == other.format && std::to_underlying(usage) == std::to_underlying(other.usage);
    }
};

class ResourcePool {
public:
    auto init(RhiDevice* device) -> void;
    auto destroy() -> void;

    auto acquireTexture(const RhiTextureDesc& desc) -> RhiTexture*;
    auto releaseTexture(const RhiTextureDesc& desc, RhiTexture* texture) -> void;
    auto flush() -> void;

private:
    RhiDevice* device = nullptr;

    struct PoolEntry {
        ResourcePoolKey key;
        RhiTexture* texture;
    };
    std::vector<PoolEntry> available;
    std::vector<PoolEntry> inUse;
};
