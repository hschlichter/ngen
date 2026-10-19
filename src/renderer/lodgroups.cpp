#include "lodgroups.h"

#include "deletionqueue.h"
#include "framegraph.h"
#include "framegraphcontext.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <algorithm>
#include <cstring>

namespace {

struct LodGroupUploadPassData {
    FgBufferHandle staging;
    FgBufferHandle table;
};

struct LodReadbackPassData {
    FgBufferHandle totals;
    FgBufferHandle states;
    FgBufferHandle readback;
};

constexpr uint32_t stateBytes = 2 * sizeof(uint32_t);

} // namespace

auto LodGroups::init(RhiDevice* rhiDevice, uint32_t frameSlots, DeletionQueue* queue) -> void {
    device = rhiDevice;
    deletionQueue = queue;
    slots.assign(frameSlots, {});
    ensureCapacity(0, 0);
}

auto LodGroups::destroy() -> void {
    for (auto* buffer : {table, states, residency}) {
        if (buffer != nullptr) {
            device->destroyBuffer(buffer);
        }
    }
    table = nullptr;
    states = nullptr;
    residency = nullptr;
    for (auto& slot : slots) {
        for (auto* buffer : {slot.staging, slot.groupCounters, slot.totals, slot.readback}) {
            if (buffer != nullptr) {
                device->destroyBuffer(buffer);
            }
        }
        slot = {};
    }
}

auto LodGroups::workgroupsFor(uint32_t count) -> uint32_t {
    return std::max(1u, (count + groupSize - 1) / groupSize);
}

auto LodGroups::ensureCapacity(uint32_t count, uint64_t frame) -> bool {
    if (table != nullptr && count <= capacity) {
        return false;
    }
    // Frames in flight may still use the old buffers.
    for (auto* buffer : {table, states, residency}) {
        deletionQueue->deferBuffer(frame, buffer);
    }
    for (auto& slot : slots) {
        for (auto* buffer : {slot.staging, slot.groupCounters, slot.totals, slot.readback}) {
            deletionQueue->deferBuffer(frame, buffer);
        }
        slot = {};
    }
    constexpr uint32_t minCapacity = 256;
    capacity = std::max({count, capacity * 2, minCapacity});
    auto storage = [&](uint64_t size, RhiBufferUsageFlags extra, const char* name) {
        return device->createBuffer({.size = size, .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferSrc | extra, .memory = RhiMemoryUsage::GpuOnly, .debugName = name});
    };
    table = storage((uint64_t) capacity * sizeof(GpuLodGroup), RhiBufferUsage::TransferDst, "gpuscene.lodgroups");
    states = storage((uint64_t) capacity * stateBytes, {}, "gpuscene.lodstate");
    residency = storage((uint64_t) capacity * sizeof(uint32_t), {}, "gpuscene.lodresidency");
    auto workgroups = workgroupsFor(capacity);
    for (auto& slot : slots) {
        slot.staging = device->createBuffer({
            .size = (uint64_t) capacity * sizeof(GpuLodGroup),
            .usage = RhiBufferUsage::TransferSrc,
            .memory = RhiMemoryUsage::CpuToGpu,
            .debugName = "gpuscene.lodgroups.staging",
        });
        slot.stagingMapped = device->mapBuffer(slot.staging);
        slot.groupCounters = storage((uint64_t) workgroups * counterCount * sizeof(uint32_t), {}, "lodselect.groupcounters");
        slot.totals = storage(sizeof(LodSelectTotals), {}, "lodselect.totals");
        slot.readback = device->createBuffer({
            .size = sizeof(LodSelectTotals) + ((uint64_t) capacity * stateBytes),
            .usage = RhiBufferUsage::TransferDst,
            .memory = RhiMemoryUsage::GpuToCpu,
            .debugName = "lodselect.readback",
        });
        slot.readbackMapped = device->mapBuffer(slot.readback);
    }
    // New contents: nothing to sync with, and every group is uploaded.
    access = {FgAccessFlags::None, FgAccessFlags::None, FgAccessFlags::None};
    dirtyFirst = 0;
    dirtyEnd = (uint32_t) groups.size();
    return true;
}

auto LodGroups::apply(std::span<const std::pair<uint32_t, RenderLodGroup>> changes, uint32_t count, uint64_t frame) -> bool {
    groups.resize(count);
    serials.resize(count, 0);
    bool replaced = ensureCapacity(count, frame);
    for (const auto& [slot, group] : changes) {
        if (slot >= count) {
            continue;
        }
        // A slot taking a group (it was free) starts afresh: the level picked for the one before isn't this one's.
        if (group.alive && !groups[slot].alive) {
            serials[slot]++;
        }
        groups[slot] = group;
        if (dirtyEnd <= dirtyFirst) {
            dirtyFirst = slot;
            dirtyEnd = slot + 1;
        } else {
            dirtyFirst = std::min(dirtyFirst, slot);
            dirtyEnd = std::max(dirtyEnd, slot + 1);
        }
    }
    return replaced;
}

auto LodGroups::record(uint32_t slot) const -> GpuLodGroup {
    const auto& group = groups[slot];
    return {
        .center = group.center,
        .radius = group.radius,
        .thresholds = group.thresholds,
        .levelCount = group.levelCount,
        .defaultLevel = group.defaultLevel,
        .forcedLevel = group.forcedLevel,
        .serial = serials[slot],
        .alive = group.alive ? 1u : 0u,
    };
}

auto LodGroups::import(FrameGraph& fg, uint32_t frameSlot) -> Handles {
    auto& slot = slots[frameSlot];
    Handles handles;
    handles.table = fg.importBuffer("lodGroups", table, {.size = (uint64_t) capacity * sizeof(GpuLodGroup), .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferDst}, access[0]);
    handles.states = fg.importBuffer("lodStates", states, {.size = (uint64_t) capacity * stateBytes, .usage = RhiBufferUsage::Storage}, access[1]);
    handles.residency = fg.importBuffer("lodResidency", residency, {.size = (uint64_t) capacity * sizeof(uint32_t), .usage = RhiBufferUsage::Storage}, access[2]);
    imported = {handles.table, handles.states, handles.residency};
    importedThisFrame = true;
    handles.groupCounters = fg.importBuffer("lodGroupCounters", slot.groupCounters, {.size = (uint64_t) workgroupsFor(capacity) * counterCount * sizeof(uint32_t), .usage = RhiBufferUsage::Storage});
    handles.totals = fg.importBuffer("lodTotals", slot.totals, {.size = sizeof(LodSelectTotals), .usage = RhiBufferUsage::Storage});
    handles.readback = fg.importBuffer("lodReadback", slot.readback, {.size = sizeof(LodSelectTotals) + ((uint64_t) capacity * stateBytes), .usage = RhiBufferUsage::TransferDst});
    dirtyEnd = std::min(dirtyEnd, (uint32_t) groups.size());
    if (dirtyEnd > dirtyFirst) {
        auto* dst = static_cast<GpuLodGroup*>(slot.stagingMapped);
        for (uint32_t g = dirtyFirst; g < dirtyEnd; g++) {
            dst[g] = record(g);
        }
        RhiBufferCopy region = {
            .srcOffset = (uint64_t) dirtyFirst * sizeof(GpuLodGroup),
            .dstOffset = (uint64_t) dirtyFirst * sizeof(GpuLodGroup),
            .size = (uint64_t) (dirtyEnd - dirtyFirst) * sizeof(GpuLodGroup),
        };
        auto staging = fg.importBuffer("lodGroupStaging", slot.staging, {.size = (uint64_t) capacity * sizeof(GpuLodGroup), .usage = RhiBufferUsage::TransferSrc});
        const auto& data = fg.addPass<LodGroupUploadPassData>(
            "LodGroupUpload",
            [&](FrameGraphBuilder& builder, LodGroupUploadPassData& pass) {
                pass.staging = builder.read(staging, FgAccessFlags::TransferSrc);
                pass.table = builder.write(handles.table, FgAccessFlags::TransferDst);
            },
            [region](FrameGraphContext& ctx, const LodGroupUploadPassData& pass) { ctx.cmd()->copyBuffer(ctx.buffer(pass.staging), ctx.buffer(pass.table), region); });
        handles.table = data.table;
        imported[0] = handles.table;
    }
    dirtyFirst = 0;
    dirtyEnd = 0;
    return handles;
}

auto LodGroups::addReadbackPass(FrameGraph& fg, const Handles& handles, uint32_t frameSlot, bool readStates) -> void {
    auto& slot = slots[frameSlot];
    slot.pending = true;
    slot.readStates = readStates && !groups.empty();
    slot.groupCount = (uint32_t) groups.size();
    if (groups.empty()) {
        // Nothing selected, nothing counted: the totals read back as zero.
        return;
    }
    RhiBufferCopy totalsRegion = {.srcOffset = 0, .dstOffset = 0, .size = sizeof(LodSelectTotals)};
    RhiBufferCopy statesRegion = {.srcOffset = 0, .dstOffset = sizeof(LodSelectTotals), .size = std::max<uint64_t>(1, slot.groupCount) * stateBytes};
    bool withStates = slot.readStates;
    fg.addPass<LodReadbackPassData>(
        "LodReadback",
        [&](FrameGraphBuilder& builder, LodReadbackPassData& data) {
            data.totals = builder.read(handles.totals, FgAccessFlags::TransferSrc);
            if (withStates) {
                data.states = builder.read(handles.states, FgAccessFlags::TransferSrc);
            }
            data.readback = builder.write(handles.readback, FgAccessFlags::TransferDst);
            builder.setSideEffects(true);
        },
        [totalsRegion, statesRegion, withStates](FrameGraphContext& ctx, const LodReadbackPassData& data) {
            auto* cmd = ctx.cmd();
            cmd->copyBuffer(ctx.buffer(data.totals), ctx.buffer(data.readback), totalsRegion);
            if (withStates) {
                cmd->copyBuffer(ctx.buffer(data.states), ctx.buffer(data.readback), statesRegion);
            }
        });
}

auto LodGroups::parseReadback(uint32_t frameSlot, uint64_t slotFrame) -> void {
    auto& slot = slots[frameSlot];
    if (!slot.pending || slot.readbackMapped == nullptr) {
        return;
    }
    slot.pending = false;
    const auto* bytes = static_cast<const std::byte*>(slot.readbackMapped);
    read.frame = slotFrame;
    read.groupCount = slot.groupCount;
    read.totals = {};
    if (slot.groupCount > 0) {
        std::memcpy(&read.totals, bytes, sizeof(LodSelectTotals));
    }
    read.hasStates = slot.readStates;
    read.drawn.clear();
    read.wanted.clear();
    if (!slot.readStates) {
        return;
    }
    const auto* state = reinterpret_cast<const uint32_t*>(bytes + sizeof(LodSelectTotals));
    read.drawn.resize(slot.groupCount);
    read.wanted.resize(slot.groupCount);
    for (uint32_t g = 0; g < slot.groupCount; g++) {
        auto levels = state[(2 * g) + 1];
        read.wanted[g] = (uint8_t) (levels & 0xFFu);
        read.drawn[g] = (uint8_t) ((levels >> 8) & 0xFFu);
    }
}

auto LodGroups::afterExecute(const FrameGraph& fg) -> void {
    if (!importedThisFrame) {
        return;
    }
    for (size_t i = 0; i < imported.size(); i++) {
        access[i] = fg.finalAccess(imported[i]);
    }
    importedThisFrame = false;
}
