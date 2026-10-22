#include "drawlists.h"

#include "deletionqueue.h"
#include "framegraph.h"
#include "framegraphcontext.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <algorithm>
#include <cstring>

auto DrawLists::init(RhiDevice* rhiDevice, uint32_t frameSlots, DeletionQueue* queue) -> void {
    device = rhiDevice;
    deletionQueue = queue;
    slots.assign(frameSlots, {});
    ensureCapacity(0, 0, 0);
}

auto DrawLists::destroy() -> void {
    for (auto& slot : slots) {
        for (auto* buffer : {slot.params, slot.visibility, slot.cullPlanes, slot.groupCounters, slot.groupOffsets, slot.commands, slot.counts, slot.readback, slot.commandsReadback}) {
            if (buffer != nullptr) {
                device->destroyBuffer(buffer);
            }
        }
        slot = {};
    }
}

auto DrawLists::groupCountFor(uint32_t instanceCount) -> uint32_t {
    return std::max(1u, (instanceCount + groupSize - 1) / groupSize);
}

auto DrawLists::cameraBucketOf(const GpuInstance& instance) -> uint32_t {
    if (!instance.alive) {
        return noBucket;
    }
    return bucket(instance.doubleSided, instance.cutout);
}

auto DrawLists::cascadeBucketOf(const GpuInstance& instance) -> uint32_t {
    if (!instance.alive) {
        return noBucket;
    }
    if (instance.primHasCutout) {
        return bucket(instance.doubleSided, instance.cutout);
    }
    if (instance.primFirst) {
        return bucket(instance.doubleSided, false);
    }
    return noBucket;
}

auto DrawLists::countInstance(const GpuInstance& instance, int delta) -> void {
    if (auto b = cameraBucketOf(instance); b != noBucket) {
        cameraBuckets[b] += delta;
    }
    if (auto b = cascadeBucketOf(instance); b != noBucket) {
        cascadeBuckets[b] += delta;
    }
}

auto DrawLists::commandsNeeded() const -> uint64_t {
    uint64_t camera = 0;
    uint64_t cascade = 0;
    for (uint32_t b = 0; b < bucketCount; b++) {
        camera += cameraBuckets[b];
        cascade += cascadeBuckets[b];
    }
    return camera + (cascade * maxShadowCascades);
}

auto DrawLists::ensureCapacity(uint32_t instanceCount, uint64_t commandCount, uint64_t frame) -> bool {
    bool instancesFit = slots[0].visibility != nullptr && instanceCount <= capacity;
    bool commandsFit = slots[0].commands != nullptr && commandCount <= commandCapacity;
    if (instancesFit && commandsFit) {
        return false;
    }
    constexpr uint32_t minCapacity = 256;
    // Frames in flight may still use the old buffers.
    auto retire = [&](RhiBuffer*& buffer) {
        deletionQueue->deferBuffer(frame, buffer);
        buffer = nullptr;
    };
    auto storage = [&](uint64_t size, RhiBufferUsageFlags extra, const char* name) {
        // TransferSrc on all of them: the capture service reads any of them back.
        return device->createBuffer({.size = size, .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferSrc | extra, .memory = RhiMemoryUsage::GpuOnly, .debugName = name});
    };
    if (!instancesFit) {
        capacity = std::max({instanceCount, capacity * 2, minCapacity});
        auto groups = groupCountFor(capacity);
        for (auto& slot : slots) {
            for (auto* buffer : {&slot.params, &slot.visibility, &slot.cullPlanes, &slot.groupCounters, &slot.groupOffsets, &slot.counts, &slot.readback}) {
                retire(*buffer);
            }
            slot.params = device->createBuffer({.size = sizeof(CullParams), .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferSrc, .memory = RhiMemoryUsage::CpuToGpu, .debugName = "cull.params"});
            slot.paramsMapped = device->mapBuffer(slot.params);
            slot.visibility = storage((uint64_t) capacity * sizeof(uint32_t), RhiBufferUsage::TransferSrc, "cull.visibility");
            slot.cullPlanes = storage((uint64_t) capacity * sizeof(uint32_t), {}, "cull.planes");
            slot.groupCounters = storage((uint64_t) groups * counterCount * sizeof(uint32_t), {}, "cull.groupcounters");
            slot.groupOffsets = storage((uint64_t) groups * regionCount * sizeof(uint32_t), {}, "cull.groupoffsets");
            slot.counts = storage(counterCount * sizeof(uint32_t), RhiBufferUsage::Indirect | RhiBufferUsage::TransferSrc, "cull.counts");
            slot.readback = device->createBuffer({.size = readbackBytes(), .usage = RhiBufferUsage::TransferDst | RhiBufferUsage::TransferSrc, .memory = RhiMemoryUsage::GpuToCpu, .debugName = "cull.readback"});
            slot.readbackMapped = device->mapBuffer(slot.readback);
            slot.pending = false;
        }
    }
    if (!commandsFit) {
        commandCapacity = std::max({commandCount, commandCapacity * 2, (uint64_t) minCapacity});
        for (auto& slot : slots) {
            retire(slot.commands);
            // The commands readback is made again at the new size when the render debugger next asks.
            retire(slot.commandsReadback);
            slot.commandsReadbackMapped = nullptr;
            slot.commands = storage(commandBytes(), RhiBufferUsage::Indirect | RhiBufferUsage::TransferSrc, "cull.commands");
            slot.pending = false;
        }
    }
    return true;
}

auto DrawLists::writeParams(uint32_t frameSlot, uint32_t instanceCount, const glm::mat4& cameraViewProj, std::span<const ShadowCascade> cascades, bool cullEnabled) -> uint32_t {
    CullParams params;
    params.instanceCount = instanceCount;
    params.viewCount = 1 + (uint32_t) std::min(cascades.size(), (size_t) maxShadowCascades);
    params.cullEnabled = cullEnabled ? 1 : 0;
    params.commandCapacity = (uint32_t) commandCapacity;
    params.groupCount = groupCountFor(instanceCount);
    // The regions one after another, each as large as the instances that can land in it.
    uint32_t next = 0;
    for (uint32_t r = 0; r < regionCount; r++) {
        auto view = r / bucketCount;
        auto b = r % bucketCount;
        auto size = (uint32_t) (view == 0 ? cameraBuckets[b] : cascadeBuckets[b]);
        frameOffsets[r] = next;
        frameCapacities[r] = size;
        params.regionOffsets[r] = next;
        next += size;
    }
    params.commandsUsed = next;
    // Normalized planes from Frustum::fromViewProj, tested per instance in instancecull.comp.
    for (uint32_t v = 0; v < params.viewCount; v++) {
        // The camera's projection is reversed-Z with no far plane; the cascades' are standard.
        auto frustum = v == 0 ? Frustum::fromViewProj(cameraViewProj, true) : Frustum::fromViewProj(cascades[v - 1].viewProj);
        for (uint32_t p = 0; p < 6; p++) {
            params.planes[(v * 6) + p] = frustum.planes[p];
        }
    }
    std::memcpy(slots[frameSlot].paramsMapped, &params, sizeof(params));
    slots[frameSlot].instanceCount = instanceCount;
    slots[frameSlot].viewCount = params.viewCount;
    slots[frameSlot].offsets = frameOffsets;
    return params.groupCount;
}

auto DrawLists::import(FrameGraph& fg, uint32_t frameSlot) -> Handles {
    const auto& slot = slots[frameSlot];
    auto groups = groupCountFor(capacity);
    return {
        .params = fg.importBuffer("cullParams", slot.params, {.size = sizeof(CullParams), .usage = RhiBufferUsage::Storage}),
        .visibility = fg.importBuffer("cullVisibility", slot.visibility, {.size = (uint64_t) capacity * sizeof(uint32_t), .usage = RhiBufferUsage::Storage}),
        .cullPlanes = fg.importBuffer("cullPlanes", slot.cullPlanes, {.size = (uint64_t) capacity * sizeof(uint32_t), .usage = RhiBufferUsage::Storage}),
        .groupCounters = fg.importBuffer("cullGroupCounters", slot.groupCounters, {.size = (uint64_t) groups * counterCount * sizeof(uint32_t), .usage = RhiBufferUsage::Storage}),
        .groupOffsets = fg.importBuffer("cullGroupOffsets", slot.groupOffsets, {.size = (uint64_t) groups * regionCount * sizeof(uint32_t), .usage = RhiBufferUsage::Storage}),
        .commands = fg.importBuffer("drawCommands", slot.commands, {.size = commandBytes(), .usage = RhiBufferUsage::Storage | RhiBufferUsage::Indirect}),
        .counts = fg.importBuffer("drawCounts", slot.counts, {.size = counterCount * sizeof(uint32_t), .usage = RhiBufferUsage::Storage | RhiBufferUsage::Indirect}),
        .readback = fg.importBuffer("cullReadback", slot.readback, {.size = readbackBytes(), .usage = RhiBufferUsage::TransferDst}),
    };
}

namespace {
struct CullReadbackPassData {
    FgBufferHandle counts;
    FgBufferHandle visibility;
    FgBufferHandle commands;
    FgBufferHandle readback;
    FgBufferHandle commandsReadback;
};
} // namespace

auto DrawLists::addReadbackPass(FrameGraph& fg, const Handles& handles, uint32_t frameSlot, bool captureCommands, uint64_t frame) -> void {
    auto& slot = slots[frameSlot];
    if (!captureCommands && slot.commandsReadback != nullptr) {
        // The render debugger closed: the slot's last commands readback was parsed before this frame (after the slot's fence), so it goes.
        deletionQueue->deferBuffer(frame, slot.commandsReadback);
        slot.commandsReadback = nullptr;
        slot.commandsReadbackMapped = nullptr;
    }
    if (captureCommands && slot.commandsReadback == nullptr) {
        slot.commandsReadback = device->createBuffer({.size = commandBytes(), .usage = RhiBufferUsage::TransferDst, .memory = RhiMemoryUsage::GpuToCpu, .debugName = "cull.readback.commands"});
        slot.commandsReadbackMapped = device->mapBuffer(slot.commandsReadback);
    }
    slot.pending = true;
    slot.capturedCommands = captureCommands;
    RhiBufferCopy countsRegion = {.srcOffset = 0, .dstOffset = 0, .size = counterCount * sizeof(uint32_t)};
    RhiBufferCopy visibilityRegion = {.srcOffset = 0, .dstOffset = readbackVisibilityOffset(), .size = std::max<uint64_t>(1, slot.instanceCount) * sizeof(uint32_t)};
    RhiBufferCopy commandsRegion = {.srcOffset = 0, .dstOffset = 0, .size = commandBytes()};
    FgBufferHandle commandsReadback;
    if (captureCommands) {
        commandsReadback = fg.importBuffer("cullReadbackCommands", slot.commandsReadback, {.size = commandBytes(), .usage = RhiBufferUsage::TransferDst});
    }
    fg.addPass<CullReadbackPassData>(
        "CullReadback",
        [&](FrameGraphBuilder& builder, CullReadbackPassData& data) {
            data.counts = builder.read(handles.counts, FgAccessFlags::TransferSrc);
            data.visibility = builder.read(handles.visibility, FgAccessFlags::TransferSrc);
            if (captureCommands) {
                data.commands = builder.read(handles.commands, FgAccessFlags::TransferSrc);
                data.commandsReadback = builder.write(commandsReadback, FgAccessFlags::TransferDst);
            }
            data.readback = builder.write(handles.readback, FgAccessFlags::TransferDst);
            builder.setSideEffects(true);
        },
        [countsRegion, visibilityRegion, commandsRegion, captureCommands](FrameGraphContext& ctx, const CullReadbackPassData& data) {
            auto* cmd = ctx.cmd();
            cmd->copyBuffer(ctx.buffer(data.counts), ctx.buffer(data.readback), countsRegion);
            cmd->copyBuffer(ctx.buffer(data.visibility), ctx.buffer(data.readback), visibilityRegion);
            if (captureCommands) {
                cmd->copyBuffer(ctx.buffer(data.commands), ctx.buffer(data.commandsReadback), commandsRegion);
            }
        });
}

auto DrawLists::parseReadback(uint32_t frameSlot, uint64_t slotFrame) -> void {
    auto& slot = slots[frameSlot];
    if (!slot.pending || slot.readbackMapped == nullptr) {
        return;
    }
    slot.pending = false;
    const auto* bytes = static_cast<const std::byte*>(slot.readbackMapped);
    latest.frame = slotFrame;
    latest.instanceCount = slot.instanceCount;
    latest.viewCount = slot.viewCount;
    std::memcpy(latest.totals.data(), bytes, counterCount * sizeof(uint32_t));

    const auto* bits = reinterpret_cast<const uint32_t*>(bytes + readbackVisibilityOffset());
    latest.cameraVisible.resize(slot.instanceCount);
    latest.viewBits.resize(slot.instanceCount);
    for (uint32_t m = 0; m < slot.instanceCount; m++) {
        latest.cameraVisible[m] = (bits[m] & 1u) != 0 ? 1 : 0;
        latest.viewBits[m] = (uint8_t) bits[m];
    }

    latest.hasCommands = slot.capturedCommands && slot.commandsReadbackMapped != nullptr;
    const auto* commands = static_cast<const RhiDrawIndexedIndirectCommand*>(slot.commandsReadbackMapped);
    for (uint32_t r = 0; r < regionCount; r++) {
        latest.commands[r].clear();
        if (latest.hasCommands) {
            // Each region at the offset the slot's frame laid it out at.
            auto count = latest.totals[counterDraws + r];
            latest.commands[r].assign(commands + slot.offsets[r], commands + slot.offsets[r] + count);
        }
    }
}

auto DrawLists::slotBuffers(uint32_t frameSlot) const -> SlotBuffers {
    const auto& slot = slots[frameSlot];
    return {
        .params = slot.params,
        .visibility = slot.visibility,
        .cullPlanes = slot.cullPlanes,
        .groupCounters = slot.groupCounters,
        .groupOffsets = slot.groupOffsets,
        .commands = slot.commands,
        .counts = slot.counts,
    };
}

auto DrawLists::commandCount() const -> uint32_t {
    uint32_t total = 0;
    for (uint32_t r = 0; r < regionCount; r++) {
        total += drawsIn(r);
    }
    return total;
}

auto DrawLists::report(FrameGraphContext& ctx, uint32_t region, std::span<const GpuInstance> instances) const -> void {
    // Camera regions draw a submesh range; cascade regions the whole mesh from index 0, except for prims with a cutout submesh.
    bool cascade = region >= bucketCount;
    for (const auto& command : latest.commands[region]) {
        auto m = command.firstInstance;
        if (m >= instances.size()) {
            continue;
        }
        const auto& inst = instances[m];
        bool wholeMesh = cascade && !inst.primHasCutout;
        ctx.logDraw({
            .instance = m,
            .mesh = inst.mesh.index,
            .material = inst.material.index,
            .prim = inst.prim,
            .indexOffset = wholeMesh ? 0 : inst.indexOffset,
            .indexCount = command.indexCount,
        });
    }
    ctx.addIndirectStats(drawsIn(region), primitivesIn(region));
}

auto DrawLists::regionName(uint32_t region) -> const char* {
    static constexpr std::array<const char*, 20> names = {
        "camera.single",
        "camera.double",
        "camera.single.cutout",
        "camera.double.cutout",
        "cascade0.single",
        "cascade0.double",
        "cascade0.single.cutout",
        "cascade0.double.cutout",
        "cascade1.single",
        "cascade1.double",
        "cascade1.single.cutout",
        "cascade1.double.cutout",
        "cascade2.single",
        "cascade2.double",
        "cascade2.single.cutout",
        "cascade2.double.cutout",
        "cascade3.single",
        "cascade3.double",
        "cascade3.single.cutout",
        "cascade3.double.cutout",
    };
    static_assert(names.size() == regionCount);
    return region < names.size() ? names[region] : "region.invalid";
}
