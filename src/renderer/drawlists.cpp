#include "drawlists.h"

#include "deletionqueue.h"
#include "framegraph.h"
#include "framegraphcontext.h"
#include "observationmacros.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <algorithm>
#include <cstring>

auto DrawLists::init(RhiDevice* rhiDevice, uint32_t frameSlots, DeletionQueue* queue) -> void {
    device = rhiDevice;
    deletionQueue = queue;
    slots.assign(frameSlots, {});
    ensureCapacity(0, 0);
}

auto DrawLists::destroy() -> void {
    for (auto& slot : slots) {
        for (auto* buffer : {slot.params, slot.visibility, slot.groupCounters, slot.groupOffsets, slot.commands, slot.counts, slot.readback}) {
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

auto DrawLists::ensureCapacity(uint32_t instanceCount, uint64_t frame) -> bool {
    if (slots[0].commands != nullptr && instanceCount <= capacity) {
        return false;
    }
    // Frames in flight may still use the old buffers.
    for (auto& slot : slots) {
        for (auto* buffer : {slot.params, slot.visibility, slot.groupCounters, slot.groupOffsets, slot.commands, slot.counts, slot.readback}) {
            deletionQueue->deferBuffer(frame, buffer);
        }
        slot = {};
    }

    constexpr uint32_t minCapacity = 256;
    capacity = std::max({instanceCount, capacity * 2, minCapacity});
    auto groups = groupCountFor(capacity);
    auto storage = [&](uint64_t size, RhiBufferUsageFlags extra) {
        return device->createBuffer({.size = size, .usage = RhiBufferUsage::Storage | extra, .memory = RhiMemoryUsage::GpuOnly});
    };
    for (auto& slot : slots) {
        slot.params = device->createBuffer({.size = sizeof(CullParams), .usage = RhiBufferUsage::Storage, .memory = RhiMemoryUsage::CpuToGpu});
        slot.paramsMapped = device->mapBuffer(slot.params);
        slot.visibility = storage((uint64_t) capacity * sizeof(uint32_t), RhiBufferUsage::TransferSrc);
        slot.groupCounters = storage((uint64_t) groups * counterCount * sizeof(uint32_t), {});
        slot.groupOffsets = storage((uint64_t) groups * regionCount * sizeof(uint32_t), {});
        slot.commands = storage((uint64_t) regionCount * capacity * sizeof(RhiDrawIndexedIndirectCommand), RhiBufferUsage::Indirect | RhiBufferUsage::TransferSrc);
        slot.counts = storage(counterCount * sizeof(uint32_t), RhiBufferUsage::Indirect | RhiBufferUsage::TransferSrc);
        slot.readback = device->createBuffer({
            .size = readbackCommandsOffset() + ((uint64_t) regionCount * capacity * sizeof(RhiDrawIndexedIndirectCommand)),
            .usage = RhiBufferUsage::TransferDst,
            .memory = RhiMemoryUsage::CpuToGpu,
        });
        slot.readbackMapped = device->mapBuffer(slot.readback);
    }
    OBS_EVENT("Render", "DrawListsCreated", "drawlists").field("capacity", (int64_t) capacity).field("regions", (int64_t) regionCount);
    return true;
}

auto DrawLists::writeParams(uint32_t frameSlot, uint32_t instanceCount, const glm::mat4& cameraViewProj, std::span<const ShadowCascade> cascades, bool cullEnabled) -> uint32_t {
    CullParams params;
    params.instanceCount = instanceCount;
    params.viewCount = 1 + (uint32_t) std::min(cascades.size(), (size_t) maxShadowCascades);
    params.cullEnabled = cullEnabled ? 1 : 0;
    params.regionCapacity = capacity;
    params.groupCount = groupCountFor(instanceCount);
    // Planes exactly as the CPU culling built them (Frustum::fromViewProj).
    for (uint32_t v = 0; v < params.viewCount; v++) {
        auto frustum = Frustum::fromViewProj(v == 0 ? cameraViewProj : cascades[v - 1].viewProj);
        for (uint32_t p = 0; p < 6; p++) {
            params.planes[(v * 6) + p] = frustum.planes[p];
        }
    }
    std::memcpy(slots[frameSlot].paramsMapped, &params, sizeof(params));
    slots[frameSlot].instanceCount = instanceCount;
    slots[frameSlot].viewCount = params.viewCount;
    return params.groupCount;
}

auto DrawLists::import(FrameGraph& fg, uint32_t frameSlot) -> Handles {
    const auto& slot = slots[frameSlot];
    auto groups = groupCountFor(capacity);
    return {
        .params = fg.importBuffer("cullParams", slot.params, {.size = sizeof(CullParams), .usage = RhiBufferUsage::Storage}),
        .visibility = fg.importBuffer("cullVisibility", slot.visibility, {.size = (uint64_t) capacity * sizeof(uint32_t), .usage = RhiBufferUsage::Storage}),
        .groupCounters = fg.importBuffer("cullGroupCounters", slot.groupCounters, {.size = (uint64_t) groups * counterCount * sizeof(uint32_t), .usage = RhiBufferUsage::Storage}),
        .groupOffsets = fg.importBuffer("cullGroupOffsets", slot.groupOffsets, {.size = (uint64_t) groups * regionCount * sizeof(uint32_t), .usage = RhiBufferUsage::Storage}),
        .commands = fg.importBuffer("drawCommands", slot.commands, {.size = (uint64_t) regionCount * capacity * sizeof(RhiDrawIndexedIndirectCommand), .usage = RhiBufferUsage::Storage | RhiBufferUsage::Indirect}),
        .counts = fg.importBuffer("drawCounts", slot.counts, {.size = counterCount * sizeof(uint32_t), .usage = RhiBufferUsage::Storage | RhiBufferUsage::Indirect}),
        .readback = fg.importBuffer("cullReadback", slot.readback, {.size = readbackCommandsOffset() + ((uint64_t) regionCount * capacity * sizeof(RhiDrawIndexedIndirectCommand)), .usage = RhiBufferUsage::TransferDst}),
    };
}

namespace {
struct CullReadbackPassData {
    FgBufferHandle counts;
    FgBufferHandle visibility;
    FgBufferHandle commands;
    FgBufferHandle readback;
};
} // namespace

auto DrawLists::addReadbackPass(FrameGraph& fg, const Handles& handles, uint32_t frameSlot, bool captureCommands) -> void {
    auto& slot = slots[frameSlot];
    slot.pending = true;
    slot.capturedCommands = captureCommands;
    RhiBufferCopy countsRegion = {.srcOffset = 0, .dstOffset = 0, .size = counterCount * sizeof(uint32_t)};
    RhiBufferCopy visibilityRegion = {.srcOffset = 0, .dstOffset = readbackVisibilityOffset(), .size = std::max<uint64_t>(1, slot.instanceCount) * sizeof(uint32_t)};
    RhiBufferCopy commandsRegion = {.srcOffset = 0, .dstOffset = readbackCommandsOffset(), .size = (uint64_t) regionCount * capacity * sizeof(RhiDrawIndexedIndirectCommand)};
    fg.addPass<CullReadbackPassData>(
        "CullReadback",
        [&](FrameGraphBuilder& builder, CullReadbackPassData& data) {
            data.counts = builder.read(handles.counts, FgAccessFlags::TransferSrc);
            data.visibility = builder.read(handles.visibility, FgAccessFlags::TransferSrc);
            if (captureCommands) {
                data.commands = builder.read(handles.commands, FgAccessFlags::TransferSrc);
            }
            data.readback = builder.write(handles.readback, FgAccessFlags::TransferDst);
            builder.setSideEffects(true);
        },
        [countsRegion, visibilityRegion, commandsRegion, captureCommands](FrameGraphContext& ctx, const CullReadbackPassData& data) {
            auto* cmd = ctx.cmd();
            cmd->copyBuffer(ctx.buffer(data.counts), ctx.buffer(data.readback), countsRegion);
            cmd->copyBuffer(ctx.buffer(data.visibility), ctx.buffer(data.readback), visibilityRegion);
            if (captureCommands) {
                cmd->copyBuffer(ctx.buffer(data.commands), ctx.buffer(data.readback), commandsRegion);
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
    for (uint32_t m = 0; m < slot.instanceCount; m++) {
        latest.cameraVisible[m] = (bits[m] & 1u) != 0 ? 1 : 0;
    }

    latest.hasCommands = slot.capturedCommands;
    const auto* commands = reinterpret_cast<const RhiDrawIndexedIndirectCommand*>(bytes + readbackCommandsOffset());
    for (uint32_t r = 0; r < regionCount; r++) {
        latest.commands[r].clear();
        if (slot.capturedCommands) {
            auto count = std::min(latest.totals[counterDraws + r], capacity);
            latest.commands[r].assign(commands + ((size_t) r * capacity), commands + ((size_t) r * capacity) + count);
        }
    }

    if (obs::bus().categoryEnabled("Render")) {
        obs::detail::Builder event("Render", "CullReadback", "cull");
        event.field("frame", (int64_t) slotFrame);
        event.field("instances", (int64_t) slot.instanceCount);
        event.field("camera_culled", (int64_t) cameraCulled());
        static constexpr std::array<const char*, maxShadowCascades> cascadeFields = {"cascade_culled_0", "cascade_culled_1", "cascade_culled_2", "cascade_culled_3"};
        for (uint32_t c = 0; c < cascadeCount(); c++) {
            event.field(cascadeFields[c], (int64_t) cascadeCulled(c));
        }
    }
}

auto DrawLists::slotBuffers(uint32_t frameSlot) const -> SlotBuffers {
    const auto& slot = slots[frameSlot];
    return {
        .params = slot.params,
        .visibility = slot.visibility,
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
    // Camera regions draw a submesh range; cascade regions the whole mesh from index 0.
    bool wholeMesh = region >= bucketCount;
    for (const auto& command : latest.commands[region]) {
        auto m = command.firstInstance;
        if (m >= instances.size()) {
            continue;
        }
        const auto& inst = instances[m];
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
