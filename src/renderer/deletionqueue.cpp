#include "deletionqueue.h"

#include "rhidevice.h"

#include <algorithm>

auto DeletionQueue::init(RhiDevice* dev) -> void {
    device = dev;
}

auto DeletionQueue::deferBuffer(uint64_t frame, RhiBuffer* buffer) -> void {
    if (buffer == nullptr) {
        return;
    }
    defer(frame, [dev = device, buffer] { dev->destroyBuffer(buffer); });
}

auto DeletionQueue::deferTexture(uint64_t frame, RhiTexture* texture) -> void {
    if (texture == nullptr) {
        return;
    }
    defer(frame, [dev = device, texture] { dev->destroyTexture(texture); });
}

auto DeletionQueue::deferDescriptorPool(uint64_t frame, RhiDescriptorPool* pool) -> void {
    if (pool == nullptr) {
        return;
    }
    defer(frame, [dev = device, pool] { dev->destroyDescriptorPool(pool); });
}

auto DeletionQueue::defer(uint64_t frame, std::function<void()> destroy) -> void {
    pending.push_back({.frame = frame, .destroy = std::move(destroy)});
}

auto DeletionQueue::flush(uint64_t completedFrame) -> void {
    auto ready = std::partition(pending.begin(), pending.end(), [completedFrame](const Entry& e) { return e.frame > completedFrame; });
    for (auto it = ready; it != pending.end(); ++it) {
        it->destroy();
    }
    pending.erase(ready, pending.end());
}

auto DeletionQueue::flushAll() -> void {
    for (auto& entry : pending) {
        entry.destroy();
    }
    pending.clear();
}
