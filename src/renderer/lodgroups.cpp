#include "lodgroups.h"

#include "deletionqueue.h"
#include "framegraph.h"
#include "framegraphcontext.h"
#include "profile.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <algorithm>
#include <bit>
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

constexpr uint32_t stateBytes = 4 * sizeof(uint32_t);

// A matrix's first three rows, as the shader's vec4 rows (glm is column-major: row i is m[0][i] … m[3][i]).
auto rowsOf(const glm::mat4& m) -> std::array<glm::vec4, 3> {
    std::array<glm::vec4, 3> rows = {};
    for (int i = 0; i < 3; i++) {
        rows[i] = glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]);
    }
    return rows;
}

} // namespace

auto LodGroups::init(RhiDevice* rhiDevice, uint32_t frameSlots, DeletionQueue* queue) -> void {
    device = rhiDevice;
    deletionQueue = queue;
    slots.assign(frameSlots, {});
    ensureCapacity(0, 0);
}

auto LodGroups::destroy() -> void {
    growth.discard(device);
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
    constexpr uint64_t minCapacity = 256;
    auto grownCapacity = [minCapacity](uint64_t needed) -> uint64_t {
        return std::min<uint64_t>(std::max(needed, minCapacity), UINT32_MAX);
    };
    auto describe = [this](uint64_t grown) -> std::vector<GrowthBufferDesc> {
        auto storage = [](uint64_t size, RhiBufferUsageFlags extra, const char* name) -> GrowthBufferDesc {
            return {.desc = {.size = size, .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferSrc | extra, .memory = RhiMemoryUsage::GpuOnly, .debugName = name}};
        };
        auto workgroups = (uint64_t) workgroupsFor((uint32_t) grown);
        std::vector<GrowthBufferDesc> descs;
        descs.push_back(storage(grown * sizeof(GpuLodGroup), RhiBufferUsage::TransferDst, "gpuscene.lodgroups"));
        descs.push_back(storage(grown * stateBytes, {}, "gpuscene.lodstate"));
        descs.push_back(storage(grown * sizeof(uint32_t), {}, "gpuscene.lodresidency"));
        for (size_t s = 0; s < slots.size(); s++) {
            descs.push_back({.desc = {.size = grown * sizeof(GpuLodGroup), .usage = RhiBufferUsage::TransferSrc, .memory = RhiMemoryUsage::CpuToGpu, .debugName = "gpuscene.lodgroups.staging"},
                             .mapped = true});
            descs.push_back(storage(workgroups * counterCount * sizeof(uint32_t), {}, "lodselect.groupcounters"));
            descs.push_back(storage(sizeof(LodSelectTotals), {}, "lodselect.totals"));
            descs.push_back({.desc = {.size = sizeof(LodSelectTotals) + (grown * stateBytes), .usage = RhiBufferUsage::TransferDst, .memory = RhiMemoryUsage::GpuToCpu, .debugName = "lodselect.readback"},
                             .mapped = true});
        }
        return descs;
    };
    auto installNow = [&](uint64_t grown, std::vector<GrownBuffer>& made) {
        install((uint32_t) grown, made, frame);
    };
    if (table == nullptr) {
        auto grown = grownCapacity(count);
        auto made = BufferGrowth::makeNow(device, describe(grown));
        installNow(grown, made);
        return true;
    }
    return growth.update(device, count, capacity, grownCapacity, describe, installNow);
}

auto LodGroups::install(uint32_t grown, std::vector<GrownBuffer>& made, uint64_t frame) -> void {
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
    capacity = grown;
    table = made[0].buffer;
    states = made[1].buffer;
    residency = made[2].buffer;
    size_t next = 3;
    for (auto& slot : slots) {
        slot.staging = made[next].buffer;
        slot.stagingMapped = made[next].mapped;
        slot.groupCounters = made[next + 1].buffer;
        slot.totals = made[next + 2].buffer;
        slot.readback = made[next + 3].buffer;
        slot.readbackMapped = made[next + 3].mapped;
        next += 4;
    }
    // New contents: nothing to sync with, and every group is uploaded.
    access = {FgAccessFlags::None, FgAccessFlags::None, FgAccessFlags::None};
    dirtyFirst = 0;
    dirtyEnd = (uint32_t) groups.size();
}

auto LodGroups::apply(std::span<const std::pair<uint32_t, RenderLodGroup>> changes, uint32_t count, uint64_t frame) -> bool {
    groups.resize(count);
    serials.resize(count, 0);
    bool replaced = ensureCapacity(count, frame);
    PROFILE_ZONE_DETAIL("CopyLodGroups", Render);
    PROFILE_ZONE_VALUE(changes.size());
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
        .toWorld = rowsOf(group.toWorld),
        .toRoot = rowsOf(group.toRoot),
        .center = group.center,
        .radius = group.radius,
        .point = group.point,
        .kind = (uint32_t) group.kind,
        .boxMin = group.boxMin,
        .hasBox = group.hasBox ? 1u : 0u,
        .boxMax = group.boxMax,
        .blendCount = group.blendCount,
        .thresholds = group.thresholds,
        .blendThresholds = group.blendThresholds,
        .levelCount = group.levelCount,
        .defaultLevel = group.defaultLevel,
        .forcedLevel = group.forcedLevel,
        .serial = serials[slot],
        .alive = group.alive ? 1u : 0u,
        .thresholdCount = group.thresholdCount,
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
            profile::Group::Upload,
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

auto LodGroups::addReadbackPass(FrameGraph& fg, const Handles& handles, uint32_t frameSlot, bool readStates, const glm::vec3& eye, float windowArea) -> void {
    auto& slot = slots[frameSlot];
    slot.pending = true;
    slot.eye = eye;
    slot.windowArea = windowArea;
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
        profile::Group::Culling,
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
    read.eye = slot.eye;
    read.windowArea = slot.windowArea;
    read.totals = {};
    if (slot.groupCount > 0) {
        std::memcpy(&read.totals, bytes, sizeof(LodSelectTotals));
    }
    read.hasStates = slot.readStates;
    read.drawn.clear();
    read.wanted.clear();
    read.fadeLevel.clear();
    read.fade.clear();
    read.measure.clear();
    read.flags.clear();
    if (!slot.readStates) {
        return;
    }
    const auto* state = reinterpret_cast<const uint32_t*>(bytes + sizeof(LodSelectTotals));
    read.drawn.resize(slot.groupCount);
    read.wanted.resize(slot.groupCount);
    read.fadeLevel.resize(slot.groupCount);
    read.fade.resize(slot.groupCount);
    read.measure.resize(slot.groupCount);
    read.flags.resize(slot.groupCount);
    for (uint32_t g = 0; g < slot.groupCount; g++) {
        auto levels = state[(4 * g) + 1];
        read.wanted[g] = (uint8_t) (levels & 0xFFu);
        read.drawn[g] = (uint8_t) ((levels >> 8) & 0xFFu);
        read.fadeLevel[g] = (uint8_t) ((levels >> 16) & 0xFFu);
        read.fade[g] = std::bit_cast<float>(state[(4 * g) + 2]);
        read.measure[g] = std::bit_cast<float>(state[(4 * g) + 3]);
        auto flash = (levels >> 25) & 0x7Fu;
        read.flags[g] = (uint8_t) ((((levels >> 24) & 1u) != 0 ? LodStateFlags::forced : 0) | (flash != 0 ? LodStateFlags::flashing : 0));
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
