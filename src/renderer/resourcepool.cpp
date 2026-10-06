#include "resourcepool.h"
#include "rhidevice.h"


auto ResourcePool::init(RhiDevice* rhiDevice) -> void {
    device = rhiDevice;
}

auto ResourcePool::destroy() -> void {
    for (auto& entry : available) {
        device->destroyTexture(entry.texture);
    }
    available.clear();

    for (auto& entry : inUse) {
        device->destroyTexture(entry.texture);
    }
    inUse.clear();
}

auto ResourcePool::flush() -> void {
    for (auto& entry : available) {
        device->destroyTexture(entry.texture);
    }
    available.clear();
}

auto ResourcePool::acquireTexture(const RhiTextureDesc& desc) -> RhiTexture* {
    ResourcePoolKey key = {
        .width = desc.width,
        .height = desc.height,
        .format = desc.format,
        .usage = desc.usage,
    };

    for (auto it = available.begin(); it != available.end(); ++it) {
        if (it->key == key) {
            auto* tex = it->texture;
            inUse.push_back(*it);
            available.erase(it);
            // A pooled texture takes the name of the resource that holds it this frame.
            if (desc.debugName != nullptr) {
                device->setDebugName(tex, desc.debugName);
            }
            return tex;
        }
    }

    auto* tex = device->createTexture(desc);
    if (tex != nullptr) {
        allocations++;
        inUse.push_back({key, tex});
    }
    return tex;
}

auto ResourcePool::releaseTexture(const RhiTextureDesc& desc, RhiTexture* texture) -> void {
    ResourcePoolKey key = {
        .width = desc.width,
        .height = desc.height,
        .format = desc.format,
        .usage = desc.usage,
    };

    for (auto it = inUse.begin(); it != inUse.end(); ++it) {
        if (it->texture == texture) {
            available.push_back(*it);
            inUse.erase(it);
            return;
        }
    }

    available.push_back({key, texture});
}
