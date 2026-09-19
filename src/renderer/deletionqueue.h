#pragma once

#include "rhitypes.h"

#include <cstdint>
#include <functional>
#include <vector>

class RhiDevice;

// Deferred GPU resource destruction. The RHI destroys immediately; the renderer
// knows which frames are in flight, so it owns the delay. Each entry is tagged
// with the frame that last used the resource; flush(completedFrame) destroys
// everything tagged with that frame or earlier.
class DeletionQueue {
public:
    auto init(RhiDevice* device) -> void;

    auto deferBuffer(uint64_t frame, RhiBuffer* buffer) -> void;
    auto deferTexture(uint64_t frame, RhiTexture* texture) -> void;
    auto deferDescriptorPool(uint64_t frame, RhiDescriptorPool* pool) -> void;
    auto defer(uint64_t frame, std::function<void()> destroy) -> void;

    // Destroy entries tagged <= completedFrame. Call after the fence for that frame is signaled.
    auto flush(uint64_t completedFrame) -> void;
    // Destroy everything. Call after RhiDevice::waitIdle at shutdown.
    auto flushAll() -> void;

private:
    struct Entry {
        uint64_t frame;
        std::function<void()> destroy;
    };

    RhiDevice* device = nullptr;
    std::vector<Entry> pending;
};
