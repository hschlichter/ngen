#pragma once

#include "rhitypes.h"

#include <cstdint>
#include <vector>

class RhiDevice;

struct ResourcePoolKey {
    uint32_t width;
    uint32_t height;
    RhiFormat format;
    RhiTextureUsageFlags usage;

    auto operator==(const ResourcePoolKey& other) const -> bool {
        return width == other.width && height == other.height && format == other.format && usage == other.usage;
    }
};

class ResourcePool {
public:
    auto init(RhiDevice* device) -> void;
    auto destroy() -> void;

    auto acquireTexture(const RhiTextureDesc& desc) -> RhiTexture*;
    auto releaseTexture(const RhiTextureDesc& desc, RhiTexture* texture) -> void;
    auto flush() -> void;

    struct Stats {
        uint32_t available = 0;
        uint32_t inUse = 0;
        uint32_t allocationsTotal = 0;
    };
    auto stats() const -> Stats { return {.available = (uint32_t) available.size(), .inUse = (uint32_t) inUse.size(), .allocationsTotal = allocations}; }
    template <typename Fn>
    auto forEach(Fn&& fn) const -> void {
        for (const auto& e : available) {
            fn(e.key, false);
        }
        for (const auto& e : inUse) {
            fn(e.key, true);
        }
    }

private:
    RhiDevice* device = nullptr;

    struct PoolEntry {
        ResourcePoolKey key;
        RhiTexture* texture;
    };
    std::vector<PoolEntry> available;
    std::vector<PoolEntry> inUse;
    uint32_t allocations = 0;
};
