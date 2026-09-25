#include "drawlists.h"

#include "deletionqueue.h"
#include "framegraph.h"
#include "framegraphcontext.h"
#include "observationmacros.h"
#include "profile.h"
#include "rhidevice.h"

#include <algorithm>
#include <cstring>

auto DrawLists::init(RhiDevice* rhiDevice, uint32_t frameSlots, DeletionQueue* queue) -> void {
    device = rhiDevice;
    deletionQueue = queue;
    commandBuffers.assign(frameSlots, nullptr);
    commandMapped.assign(frameSlots, nullptr);
    countBuffers.assign(frameSlots, nullptr);
    countMapped.assign(frameSlots, nullptr);
    ensureCapacity(0, 0);
}

auto DrawLists::destroy() -> void {
    for (auto*& buffer : commandBuffers) {
        device->destroyBuffer(buffer);
        buffer = nullptr;
    }
    for (auto*& buffer : countBuffers) {
        device->destroyBuffer(buffer);
        buffer = nullptr;
    }
}

auto DrawLists::ensureCapacity(uint32_t count, uint64_t frame) -> void {
    if (commandBuffers[0] != nullptr && count <= capacity) {
        return;
    }
    // Frames in flight may still read the old buffers.
    for (auto*& buffer : commandBuffers) {
        deletionQueue->deferBuffer(frame, buffer);
        buffer = nullptr;
    }
    for (auto*& buffer : countBuffers) {
        deletionQueue->deferBuffer(frame, buffer);
        buffer = nullptr;
    }

    constexpr uint32_t minCapacity = 256;
    capacity = std::max({count, capacity * 2, minCapacity});
    for (size_t i = 0; i < commandBuffers.size(); i++) {
        commandBuffers[i] = device->createBuffer({
            .size = (uint64_t) regionCount * capacity * sizeof(RhiDrawIndexedIndirectCommand),
            .usage = RhiBufferUsage::Indirect,
            .memory = RhiMemoryUsage::CpuToGpu,
        });
        commandMapped[i] = device->mapBuffer(commandBuffers[i]);
        countBuffers[i] = device->createBuffer({
            .size = regionCount * sizeof(uint32_t),
            .usage = RhiBufferUsage::Indirect,
            .memory = RhiMemoryUsage::CpuToGpu,
        });
        countMapped[i] = device->mapBuffer(countBuffers[i]);
    }
    OBS_EVENT("Render", "DrawListsCreated", "drawlists").field("capacity", (int64_t) capacity).field("regions", (int64_t) regionCount);
}

auto DrawLists::build(std::span<const GpuInstance> instances,
                      std::span<const uint8_t> visible,
                      const std::array<std::vector<uint8_t>, maxShadowCascades>& shadowVisible,
                      uint32_t cascadeCount,
                      const GpuScene& scene,
                      uint32_t frameSlot,
                      uint64_t frame) -> void {
    PROFILE_ZONE("BuildDrawLists");
    auto instanceCount = (uint32_t) instances.size();
    ensureCapacity(instanceCount, frame);
    for (uint32_t r = 0; r < regionCount; r++) {
        mirrorCommands[r].clear();
        mirrorInstances[r].clear();
    }

    // Camera: every visible instance, its submesh range. The geometry pass and the depth
    // prepass share these regions; the vertex and position pools share vertexOffset.
    bool useVisible = visible.size() == instanceCount;
    for (uint32_t m = 0; m < instanceCount; m++) {
        const auto& inst = instances[m];
        if (useVisible && visible[m] == 0) {
            continue;
        }
        const auto* range = scene.meshRange(inst.mesh.index);
        if (range == nullptr) {
            continue;
        }
        auto region = cameraRegion(inst.doubleSided);
        mirrorCommands[region].push_back({
            .indexCount = inst.indexCount,
            .instanceCount = 1,
            .firstIndex = range->firstIndex + inst.indexOffset,
            .vertexOffset = range->vertexOffset,
            .firstInstance = m,
        });
        mirrorInstances[region].push_back(m);
    }

    // Cascades: shadows are material-agnostic, so the whole mesh once, on the prim's first
    // submesh instance.
    for (uint32_t c = 0; c < cascadeCount && c < maxShadowCascades; c++) {
        const auto& mask = shadowVisible[c];
        bool useMask = mask.size() == instanceCount;
        for (uint32_t m = 0; m < instanceCount; m++) {
            const auto& inst = instances[m];
            if (!inst.primFirst) {
                continue;
            }
            if (useMask && mask[m] == 0) {
                continue;
            }
            const auto* range = scene.meshRange(inst.mesh.index);
            if (range == nullptr) {
                continue;
            }
            auto region = cascadeRegion(c, inst.doubleSided);
            mirrorCommands[region].push_back({
                .indexCount = range->indexCount,
                .instanceCount = 1,
                .firstIndex = range->firstIndex,
                .vertexOffset = range->vertexOffset,
                .firstInstance = m,
            });
            mirrorInstances[region].push_back(m);
        }
    }

    auto* commands = static_cast<RhiDrawIndexedIndirectCommand*>(commandMapped[frameSlot]);
    auto* counts = static_cast<uint32_t*>(countMapped[frameSlot]);
    for (uint32_t r = 0; r < regionCount; r++) {
        const auto& list = mirrorCommands[r];
        std::memcpy(commands + ((size_t) r * capacity), list.data(), list.size() * sizeof(RhiDrawIndexedIndirectCommand));
        counts[r] = (uint32_t) list.size();
    }
}

auto DrawLists::import(FrameGraph& fg, uint32_t frameSlot) -> Handles {
    // The slot's fence guards these buffers, like the slot's UBO: no earlier access to sync with.
    return {
        .commands = fg.importBuffer("drawCommands", commandBuffers[frameSlot], {.size = (uint64_t) regionCount * capacity * sizeof(RhiDrawIndexedIndirectCommand), .usage = RhiBufferUsage::Indirect}),
        .counts = fg.importBuffer("drawCounts", countBuffers[frameSlot], {.size = regionCount * sizeof(uint32_t), .usage = RhiBufferUsage::Indirect}),
    };
}

auto DrawLists::primitivesIn(uint32_t region) const -> uint64_t {
    uint64_t primitives = 0;
    for (const auto& command : mirrorCommands[region]) {
        primitives += command.indexCount / 3;
    }
    return primitives;
}

auto DrawLists::commandCount() const -> uint32_t {
    uint32_t total = 0;
    for (const auto& list : mirrorCommands) {
        total += (uint32_t) list.size();
    }
    return total;
}

auto DrawLists::report(FrameGraphContext& ctx, uint32_t region, std::span<const GpuInstance> instances) const -> void {
    const auto& commands = mirrorCommands[region];
    const auto& owners = mirrorInstances[region];
    // Camera regions draw a submesh range; cascade regions the whole mesh from index 0.
    bool wholeMesh = region >= bucketCount;
    for (size_t i = 0; i < commands.size(); i++) {
        const auto& inst = instances[owners[i]];
        ctx.logDraw({
            .instance = owners[i],
            .mesh = inst.mesh.index,
            .material = inst.material.index,
            .prim = inst.prim,
            .indexOffset = wholeMesh ? 0 : inst.indexOffset,
            .indexCount = commands[i].indexCount,
        });
    }
    ctx.addIndirectStats((uint32_t) commands.size(), primitivesIn(region));
}
