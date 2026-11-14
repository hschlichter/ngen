#include "drawlists.h"

#include "deletionqueue.h"
#include "framegraph.h"
#include "framegraphcontext.h"
#include "jobsystem.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <algorithm>
#include <cstring>

auto DrawLists::init(RhiDevice* rhiDevice, uint32_t frameSlots, DeletionQueue* queue) -> void {
    device = rhiDevice;
    deletionQueue = queue;
    slots.assign(frameSlots, {});
    ensureCapacity(0, 0, 0);
    ensureWorkCapacity(0, 0);
}

auto DrawLists::destroy() -> void {
    instanceGrowth.discard(device);
    commandGrowth.discard(device);
    workGrowth.discard(device);
    if (history != nullptr) {
        device->destroyBuffer(history);
        history = nullptr;
    }
    for (auto& slot : slots) {
        for (auto* buffer :
             {slot.params, slot.groupBounds, slot.visibility, slot.cullPlanes, slot.groupCounters, slot.groupOffsets, slot.commands, slot.counts, slot.meshletStats, slot.regionWork, slot.workItems, slot.readback, slot.commandsReadback}) {
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

auto DrawLists::bucketOf(const GpuInstance& instance) -> uint32_t {
    if (!instance.alive) {
        return noBucket;
    }
    return bucket(instance.doubleSided, instance.cutout);
}

// An instance of a render-LOD group counts in its cutout bucket as well as its own: while its group fades, the cull pass draws it there.
auto DrawLists::countInstance(const GpuInstance& instance, int delta) -> void {
    Counts counts;
    countInto(instance, delta, counts);
    addCounts(counts);
}

auto DrawLists::countInto(const GpuInstance& instance, int delta, Counts& counts) -> void {
    auto b = bucketOf(instance);
    if (b == noBucket) {
        return;
    }
    auto sign = delta > 0 ? 1 : -1;
    counts.buckets[b] += sign;
    // A render-LOD instance in an opaque bucket may fade: it is counted in the cutout bucket too.
    if (instance.lodGroup != 0 && b < 2) {
        counts.buckets[b + 2] += sign;
    }
    counts.meshlets += sign * (int64_t) instance.meshletCount;
}

auto DrawLists::addCounts(const Counts& counts) -> void {
    for (uint32_t b = 0; b < bucketCount; b++) {
        buckets[b] = (uint64_t) ((int64_t) buckets[b] + counts.buckets[b]);
    }
    liveMeshletCount = (uint64_t) ((int64_t) liveMeshletCount + counts.meshlets);
}

auto DrawLists::updateGroupBounds(const ChunkedList<GpuInstance>& instances, std::span<const uint32_t> changedSlots) -> void {
    auto groups = groupCountFor((uint32_t) instances.size());
    std::vector<uint8_t> dirty(groups, 0);
    // A change in the slot count moves which slots the last groups hold.
    if (instances.size() != groupSlotCount) {
        auto from = std::min(instances.size(), groupSlotCount) / groupSize;
        for (size_t g = from; g < groups; g++) {
            dirty[g] = 1;
        }
    }
    if (groupBoxes.size() != groups) {
        for (size_t g = std::min(groupBoxes.size(), (size_t) groups); g < groups; g++) {
            dirty[g] = 1;
        }
        groupBoxes.resize(groups);
    }
    for (auto slot : changedSlots) {
        if (slot < instances.size()) {
            dirty[slot / groupSize] = 1;
        }
    }
    groupSlotCount = instances.size();
    std::vector<uint32_t> dirtyGroups;
    for (uint32_t g = 0; g < groups; g++) {
        if (dirty[g] != 0) {
            dirtyGroups.push_back(g);
        }
    }
    // On the workers: each group's box from its own slots.
    JobSystem::parallelFor(
        dirtyGroups.size(),
        256,
        [&](size_t begin, size_t end) {
            for (size_t d = begin; d < end; d++) {
                auto g = dirtyGroups[d];
                AABB box = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
                bool unbounded = false;
                uint32_t live = 0;
                auto slotEnd = std::min<size_t>(((size_t) g + 1) * groupSize, instances.size());
                for (size_t m = (size_t) g * groupSize; m < slotEnd; m++) {
                    const auto& instance = instances[m];
                    if (!instance.alive) {
                        continue;
                    }
                    live++;
                    if (!instance.worldBounds.valid()) {
                        unbounded = true;
                        continue;
                    }
                    box.min = glm::min(box.min, instance.worldBounds.min);
                    box.max = glm::max(box.max, instance.worldBounds.max);
                }
                float kind = groupBoxTested;
                if (live == 0) {
                    kind = groupBoxEmpty;
                } else if (unbounded || !box.valid()) {
                    kind = groupBoxUnbounded;
                }
                groupBoxes[g] = {
                    .min = glm::vec4(box.valid() ? box.min : glm::vec3(0.0f), kind),
                    .max = glm::vec4(box.valid() ? box.max : glm::vec3(0.0f), (float) live),
                };
            }
        },
        "GroupBoundsRange");
}

auto DrawLists::commandsNeeded() const -> uint64_t {
    uint64_t live = 0;
    for (uint32_t b = 0; b < bucketCount; b++) {
        live += buckets[b];
    }
    return live * viewCount;
}

auto DrawLists::ensureCapacity(uint32_t instanceCount, uint64_t commandCount, uint64_t frame) -> bool {
    constexpr uint64_t minCapacity = 256;
    bool replaced = false;

    // Per instance: the history, and every slot's culling buffers, grown together. The parameters, counters and readback are made again
    // with them.
    auto instanceDescs = [this](uint64_t grown) -> std::vector<GrowthBufferDesc> {
        auto groups = (uint64_t) groupCountFor((uint32_t) grown);
        auto storage = [](uint64_t size, RhiBufferUsageFlags extra, const char* name) -> GrowthBufferDesc {
            // TransferSrc on all of them: the capture service reads any of them back.
            return {.desc = {.size = size, .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferSrc | extra, .memory = RhiMemoryUsage::GpuOnly, .debugName = name}};
        };
        std::vector<GrowthBufferDesc> descs;
        descs.push_back(storage(grown * sizeof(uint32_t), {}, "cull.history"));
        for (size_t s = 0; s < slots.size(); s++) {
            descs.push_back({.desc = {.size = sizeof(CullParams), .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferSrc, .memory = RhiMemoryUsage::CpuToGpu, .debugName = "cull.params"},
                             .mapped = true});
            descs.push_back({.desc = {.size = groups * sizeof(GroupBox), .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferSrc, .memory = RhiMemoryUsage::CpuToGpu, .debugName = "cull.groupbounds"},
                             .mapped = true});
            descs.push_back(storage(grown * sizeof(uint32_t), RhiBufferUsage::TransferSrc, "cull.visibility"));
            descs.push_back(storage(grown * sizeof(uint32_t), {}, "cull.planes"));
            descs.push_back(storage(groups * counterCount * sizeof(uint32_t), {}, "cull.groupcounters"));
            // Per workgroup and region: where its draw records start, then where its work items start.
            descs.push_back(storage(groups * regionCount * 2 * sizeof(uint32_t), {}, "cull.groupoffsets"));
            descs.push_back(storage(counterCount * sizeof(uint32_t), RhiBufferUsage::Indirect | RhiBufferUsage::TransferSrc, "cull.counts"));
            descs.push_back(storage(meshletCounterCount * sizeof(uint32_t), RhiBufferUsage::TransferSrc, "cull.meshletstats"));
            descs.push_back(storage(regionCount * sizeof(RegionWork), RhiBufferUsage::Indirect, "cull.regionwork"));
            descs.push_back({.desc = {.size = readbackBytesFor(grown), .usage = RhiBufferUsage::TransferDst | RhiBufferUsage::TransferSrc, .memory = RhiMemoryUsage::GpuToCpu, .debugName = "cull.readback"},
                             .mapped = true});
        }
        return descs;
    };
    auto grownInstances = [minCapacity](uint64_t needed) -> uint64_t {
        return std::min<uint64_t>(std::max(needed, minCapacity), UINT32_MAX);
    };
    auto installInstancesNow = [&](uint64_t grown, std::vector<GrownBuffer>& made) {
        installInstances((uint32_t) grown, made, frame);
    };
    if (slots[0].visibility == nullptr) {
        auto grown = grownInstances(instanceCount);
        auto made = BufferGrowth::makeNow(device, instanceDescs(grown));
        installInstancesNow(grown, made);
        replaced = true;
    } else {
        replaced |= instanceGrowth.update(device, instanceCount, capacity, grownInstances, instanceDescs, installInstancesNow);
    }

    // The draw records, every region's, per slot.
    auto commandDescs = [this](uint64_t grown) -> std::vector<GrowthBufferDesc> {
        std::vector<GrowthBufferDesc> descs;
        for (size_t s = 0; s < slots.size(); s++) {
            descs.push_back({.desc = {.size = grown * sizeof(DrawRecord),
                                      .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferSrc,
                                      .memory = RhiMemoryUsage::GpuOnly,
                                      .debugName = "cull.commands"}});
        }
        return descs;
    };
    auto grownCommands = [minCapacity](uint64_t needed) -> uint64_t {
        return std::max(needed, minCapacity);
    };
    auto installCommandsNow = [&](uint64_t grown, std::vector<GrownBuffer>& made) {
        installCommands(grown, made, frame);
    };
    if (slots[0].commands == nullptr) {
        auto grown = grownCommands(commandCount);
        auto made = BufferGrowth::makeNow(device, commandDescs(grown));
        installCommandsNow(grown, made);
        replaced = true;
    } else {
        replaced |= commandGrowth.update(device, commandCount, commandCapacity, grownCommands, commandDescs, installCommandsNow);
    }
    return replaced;
}

auto DrawLists::installInstances(uint32_t grown, std::vector<GrownBuffer>& made, uint64_t frame) -> void {
    // Frames in flight may still use the old buffers.
    auto retire = [&](RhiBuffer*& buffer) {
        deletionQueue->deferBuffer(frame, buffer);
        buffer = nullptr;
    };
    size_t next = 0;
    auto take = [&](RhiBuffer*& buffer) {
        retire(buffer);
        buffer = made[next].buffer;
        return made[next++].mapped;
    };
    capacity = grown;
    // A new history is undefined until the culling clears it; until then occlusion treats nothing as seen.
    take(history);
    historyAccess = FgAccessFlags::None;
    historyFresh = true;
    for (auto& slot : slots) {
        slot.paramsMapped = take(slot.params);
        slot.groupBoundsMapped = take(slot.groupBounds);
        take(slot.visibility);
        take(slot.cullPlanes);
        take(slot.groupCounters);
        take(slot.groupOffsets);
        take(slot.counts);
        take(slot.meshletStats);
        take(slot.regionWork);
        slot.readbackMapped = take(slot.readback);
        slot.pending = false;
    }
}

auto DrawLists::installCommands(uint64_t grown, std::vector<GrownBuffer>& made, uint64_t frame) -> void {
    commandCapacity = grown;
    for (size_t s = 0; s < slots.size(); s++) {
        auto& slot = slots[s];
        deletionQueue->deferBuffer(frame, slot.commands);
        // The commands readback is made again at the new size when the render debugger next asks.
        deletionQueue->deferBuffer(frame, slot.commandsReadback);
        slot.commandsReadback = nullptr;
        slot.commandsReadbackMapped = nullptr;
        slot.commands = made[s].buffer;
        slot.pending = false;
    }
}

auto DrawLists::ensureWorkCapacity(uint64_t items, uint64_t frame) -> bool {
    // What the last readback couldn't hold: the frame drew without those meshlets, so grow past it.
    uint64_t wanted = items;
    if (auto overflow = latest.totals[counterWorkOverflow]; overflow > 0) {
        wanted = std::max<uint64_t>(wanted, (uint64_t) workItemCapacity + overflow);
    }
    constexpr uint64_t minItems = 64 * 1024;
    auto grownWork = [this, minItems](uint64_t needed) -> uint64_t {
        auto grown = std::max(needed, minItems);
        // At most the largest buffer the device makes: past it, nothing to grow to (the caller reports it).
        grown = std::min<uint64_t>(grown, device->limits().maxBufferBytes / sizeof(MeshletWorkItem));
        return std::min<uint64_t>(grown, UINT32_MAX);
    };
    auto workDescs = [this](uint64_t grown) -> std::vector<GrowthBufferDesc> {
        std::vector<GrowthBufferDesc> descs;
        for (size_t s = 0; s < slots.size(); s++) {
            descs.push_back({.desc = {.size = grown * sizeof(MeshletWorkItem),
                                      .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferSrc,
                                      .memory = RhiMemoryUsage::GpuOnly,
                                      .debugName = "cull.workitems"}});
        }
        return descs;
    };
    auto installWorkNow = [&](uint64_t grown, std::vector<GrownBuffer>& made) {
        installWork((uint32_t) grown, made, frame);
    };
    if (slots[0].workItems == nullptr) {
        auto grown = grownWork(wanted);
        auto made = BufferGrowth::makeNow(device, workDescs(grown));
        installWorkNow(grown, made);
        return true;
    }
    return workGrowth.update(device, wanted, workItemCapacity, grownWork, workDescs, installWorkNow);
}

auto DrawLists::installWork(uint32_t grown, std::vector<GrownBuffer>& made, uint64_t frame) -> void {
    workItemCapacity = grown;
    for (size_t s = 0; s < slots.size(); s++) {
        deletionQueue->deferBuffer(frame, slots[s].workItems);
        slots[s].workItems = made[s].buffer;
    }
    // The overflow is answered: don't grow for it again.
    latest.totals[counterWorkOverflow] = 0;
}

auto DrawLists::growthStatus() const -> std::array<GrowthStatus, 3> {
    return {
        instanceGrowth.statusOf(capacity, groupSlotCount),
        commandGrowth.statusOf(commandCapacity, commandsNeeded()),
        workGrowth.statusOf(workItemCapacity, liveMeshletCount * 2),
    };
}

auto DrawLists::writeParams(uint32_t frameSlot, uint32_t instanceCount, const glm::mat4& cameraViewProj, std::span<const ShadowCascade> cascades, bool cullEnabled, Occlusion occlusion)
    -> uint32_t {
    CullParams params;
    params.instanceCount = instanceCount;
    params.viewCount = 1 + (uint32_t) std::min(cascades.size(), (size_t) maxShadowCascades);
    params.cullEnabled = cullEnabled ? 1 : 0;
    params.commandCapacity = (uint32_t) commandCapacity;
    params.groupCount = groupCountFor(instanceCount);
    // The regions one after another, each as large as the instances of its bucket: every view sees an instance in the same bucket. While
    // the commands wait for a growth, each region is cut to its share of the capacity; the cull drops (and counts) what doesn't fit.
    uint64_t wanted = 0;
    for (uint32_t r = 0; r < regionCount; r++) {
        wanted += buckets[r % bucketCount];
    }
    uint32_t next = 0;
    for (uint32_t r = 0; r < regionCount; r++) {
        auto size = (uint32_t) buckets[r % bucketCount];
        if (wanted > commandCapacity) {
            size = (uint32_t) ((uint64_t) size * commandCapacity / wanted);
        }
        frameOffsets[r] = next;
        frameCapacities[r] = size;
        params.regionOffsets[r] = next;
        next += size;
    }
    params.commandsUsed = next;
    params.occlusion = occlusion.enabled ? 1 : 0;
    params.hzbWidth = occlusion.hzbWidth;
    params.hzbHeight = occlusion.hzbHeight;
    params.workCapacity = workItemCapacity;
    params.cameraViewProj = cameraViewProj;
    // The culling camera's position: the point its view-projection maps to w = 0 with x = y = 0 (reversed-Z, no far plane: clip (0, 0, 1, 0)).
    auto eye = glm::inverse(cameraViewProj) * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    params.cameraPosition = std::abs(eye.w) > 1e-12f ? glm::vec4(glm::vec3(eye) / eye.w, 1.0f) : glm::vec4(0.0f);
    // Normalized planes from Frustum::fromViewProj, tested per instance in instancecull.comp.
    for (uint32_t v = 0; v < params.viewCount; v++) {
        // The camera's projection is reversed-Z with no far plane; the cascades' are standard.
        auto frustum = v == 0 ? Frustum::fromViewProj(cameraViewProj, true) : Frustum::fromViewProj(cascades[v - 1].viewProj);
        for (uint32_t p = 0; p < 6; p++) {
            params.planes[(v * 6) + p] = frustum.planes[p];
        }
    }
    // The late view culls as the camera does.
    for (uint32_t p = 0; p < 6; p++) {
        params.planes[(lateView * 6) + p] = params.planes[p];
    }
    std::memcpy(slots[frameSlot].paramsMapped, &params, sizeof(params));
    // The groups' boxes as the slots are now; a group past the boxes computed (none yet) is tested slot by slot.
    auto* boxes = static_cast<GroupBox*>(slots[frameSlot].groupBoundsMapped);
    for (uint32_t g = 0; g < params.groupCount; g++) {
        boxes[g] = g < groupBoxes.size() ? groupBoxes[g] : GroupBox{.min = glm::vec4(0.0f, 0.0f, 0.0f, groupBoxUnbounded)};
    }
    slots[frameSlot].instanceCount = instanceCount;
    slots[frameSlot].viewCount = params.viewCount;
    slots[frameSlot].occlusion = occlusion.enabled;
    slots[frameSlot].occlusionCullCamera = occlusion.enabled && occlusion.cullCamera;
    slots[frameSlot].workCapacity = workItemCapacity;
    slots[frameSlot].offsets = frameOffsets;
    slots[frameSlot].capacities = frameCapacities;
    return params.groupCount;
}

auto DrawLists::import(FrameGraph& fg, uint32_t frameSlot) -> Handles {
    const auto& slot = slots[frameSlot];
    auto groups = groupCountFor(capacity);
    return {
        .params = fg.importBuffer("cullParams", slot.params, {.size = sizeof(CullParams), .usage = RhiBufferUsage::Storage}),
        .groupBounds = fg.importBuffer("cullGroupBounds", slot.groupBounds, {.size = (uint64_t) groups * sizeof(GroupBox), .usage = RhiBufferUsage::Storage}),
        .visibility = fg.importBuffer("cullVisibility", slot.visibility, {.size = (uint64_t) capacity * sizeof(uint32_t), .usage = RhiBufferUsage::Storage}),
        .cullPlanes = fg.importBuffer("cullPlanes", slot.cullPlanes, {.size = (uint64_t) capacity * sizeof(uint32_t), .usage = RhiBufferUsage::Storage}),
        .groupCounters = fg.importBuffer("cullGroupCounters", slot.groupCounters, {.size = (uint64_t) groups * counterCount * sizeof(uint32_t), .usage = RhiBufferUsage::Storage}),
        .groupOffsets = fg.importBuffer("cullGroupOffsets", slot.groupOffsets, {.size = (uint64_t) groups * regionCount * 2 * sizeof(uint32_t), .usage = RhiBufferUsage::Storage}),
        .commands = fg.importBuffer("drawCommands", slot.commands, {.size = commandBytes(), .usage = RhiBufferUsage::Storage}),
        .counts = fg.importBuffer("drawCounts", slot.counts, {.size = counterCount * sizeof(uint32_t), .usage = RhiBufferUsage::Storage | RhiBufferUsage::Indirect}),
        .meshletStats = fg.importBuffer("cullMeshletStats", slot.meshletStats, {.size = meshletCounterCount * sizeof(uint32_t), .usage = RhiBufferUsage::Storage}),
        .readback = fg.importBuffer("cullReadback", slot.readback, {.size = readbackBytes(), .usage = RhiBufferUsage::TransferDst}),
        .history = importHistory(fg),
        .regionWork = fg.importBuffer("cullRegionWork", slot.regionWork, {.size = regionCount * sizeof(RegionWork), .usage = RhiBufferUsage::Storage | RhiBufferUsage::Indirect}),
        .workItems = fg.importBuffer("cullWorkItems", slot.workItems, {.size = (uint64_t) workItemCapacity * sizeof(MeshletWorkItem), .usage = RhiBufferUsage::Storage}),
    };
}

auto DrawLists::importHistory(FrameGraph& fg) -> FgBufferHandle {
    historyImport = fg.importBuffer("cullHistory", history, {.size = (uint64_t) capacity * sizeof(uint32_t), .usage = RhiBufferUsage::Storage}, historyAccess);
    historyImported = true;
    return historyImport;
}

auto DrawLists::afterExecute(const FrameGraph& fg) -> void {
    if (!historyImported) {
        return;
    }
    historyAccess = fg.finalAccess(historyImport);
    historyImported = false;
}

namespace {
struct CullReadbackPassData {
    FgBufferHandle counts;
    FgBufferHandle meshletStats;
    FgBufferHandle visibility;
    FgBufferHandle commands;
    FgBufferHandle readback;
    FgBufferHandle commandsReadback;
};
} // namespace

auto DrawLists::addReadbackPass(FrameGraph& fg, const Handles& handles, uint32_t frameSlot, bool readVisibility, bool captureCommands, uint64_t frame) -> void {
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
    slot.readVisibility = readVisibility;
    slot.capturedCommands = captureCommands;
    RhiBufferCopy countsRegion = {.srcOffset = 0, .dstOffset = 0, .size = counterCount * sizeof(uint32_t)};
    RhiBufferCopy meshletRegion = {.srcOffset = 0, .dstOffset = readbackMeshletOffset(), .size = meshletCounterCount * sizeof(uint32_t)};
    RhiBufferCopy visibilityRegion = {.srcOffset = 0, .dstOffset = readbackVisibilityOffset(), .size = std::max<uint64_t>(1, slot.instanceCount) * sizeof(uint32_t)};
    RhiBufferCopy commandsRegion = {.srcOffset = 0, .dstOffset = 0, .size = commandBytes()};
    FgBufferHandle commandsReadback;
    if (captureCommands) {
        commandsReadback = fg.importBuffer("cullReadbackCommands", slot.commandsReadback, {.size = commandBytes(), .usage = RhiBufferUsage::TransferDst});
    }
    fg.addPass<CullReadbackPassData>(
        "CullReadback",
        profile::Group::Culling,
        [&](FrameGraphBuilder& builder, CullReadbackPassData& data) {
            data.counts = builder.read(handles.counts, FgAccessFlags::TransferSrc);
            data.meshletStats = builder.read(handles.meshletStats, FgAccessFlags::TransferSrc);
            if (readVisibility) {
                data.visibility = builder.read(handles.visibility, FgAccessFlags::TransferSrc);
            }
            if (captureCommands) {
                data.commands = builder.read(handles.commands, FgAccessFlags::TransferSrc);
                data.commandsReadback = builder.write(commandsReadback, FgAccessFlags::TransferDst);
            }
            data.readback = builder.write(handles.readback, FgAccessFlags::TransferDst);
            builder.setSideEffects(true);
        },
        [countsRegion, meshletRegion, visibilityRegion, commandsRegion, readVisibility, captureCommands](FrameGraphContext& ctx, const CullReadbackPassData& data) {
            auto* cmd = ctx.cmd();
            cmd->copyBuffer(ctx.buffer(data.counts), ctx.buffer(data.readback), countsRegion);
            cmd->copyBuffer(ctx.buffer(data.meshletStats), ctx.buffer(data.readback), meshletRegion);
            if (readVisibility) {
                cmd->copyBuffer(ctx.buffer(data.visibility), ctx.buffer(data.readback), visibilityRegion);
            }
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
    latest.occlusion = slot.occlusion;
    latest.occlusionCullCamera = slot.occlusionCullCamera;
    std::memcpy(latest.totals.data(), bytes, counterCount * sizeof(uint32_t));
    std::memcpy(latest.meshletStats.data(), bytes + readbackMeshletOffset(), meshletCounterCount * sizeof(uint32_t));
    // The frames in flight when the buffer grew report the overflow it grew for; only a frame built with the current buffer counts.
    if (slot.workCapacity < workItemCapacity) {
        latest.totals[counterWorkOverflow] = 0;
    }

    // The visibility bits only when the frame asked for them (the culled overlay): at 2.4 million instances they cost milliseconds a frame.
    latest.viewBits.clear();
    if (slot.readVisibility) {
        const auto* bits = reinterpret_cast<const uint32_t*>(bytes + readbackVisibilityOffset());
        latest.viewBits.resize(slot.instanceCount);
        for (uint32_t m = 0; m < slot.instanceCount; m++) {
            latest.viewBits[m] = (uint8_t) bits[m];
        }
    }

    latest.hasCommands = slot.capturedCommands && slot.commandsReadbackMapped != nullptr;
    const auto* commands = static_cast<const DrawRecord*>(slot.commandsReadbackMapped);
    for (uint32_t r = 0; r < regionCount; r++) {
        latest.commands[r].clear();
        if (latest.hasCommands) {
            // Each region at the offset the slot's frame laid it out at; what overflowed it was never written.
            auto count = std::min(latest.totals[counterDraws + r], slot.capacities[r]);
            latest.commands[r].assign(commands + slot.offsets[r], commands + slot.offsets[r] + count);
        }
    }
}

auto DrawLists::slotBuffers(uint32_t frameSlot) const -> SlotBuffers {
    const auto& slot = slots[frameSlot];
    return {
        .params = slot.params,
        .groupBounds = slot.groupBounds,
        .visibility = slot.visibility,
        .cullPlanes = slot.cullPlanes,
        .groupCounters = slot.groupCounters,
        .groupOffsets = slot.groupOffsets,
        .commands = slot.commands,
        .counts = slot.counts,
        .meshletStats = slot.meshletStats,
        .regionWork = slot.regionWork,
        .workItems = slot.workItems,
    };
}

auto DrawLists::commandCount() const -> uint32_t {
    uint32_t total = 0;
    for (uint32_t r = 0; r < regionCount; r++) {
        total += drawsIn(r);
    }
    return total;
}

auto DrawLists::report(FrameGraphContext& ctx, uint32_t region, const ChunkedList<GpuInstance>& instances) const -> void {
    // Every region draws each instance's submesh.
    for (const auto& command : latest.commands[region]) {
        auto m = command.instance;
        if (m >= instances.size()) {
            continue;
        }
        const auto& inst = instances[m];
        ctx.logDraw({
            .instance = m,
            .mesh = inst.mesh.index,
            .material = inst.material.index,
            .prim = inst.prim,
            .indexOffset = inst.indexOffset,
            .indexCount = inst.indexCount,
            .meshlets = command.meshletCount,
        });
    }
    ctx.addIndirectStats(drawsIn(region), primitivesIn(region));
}

auto DrawLists::drawRegion(RhiCommandBuffer* cmd, RhiBuffer* regionWork, uint32_t region) -> void {
    cmd->drawMeshTasksIndirect(regionWork, (uint64_t) region * sizeof(RegionWork), 1, sizeof(RegionWork));
}

auto DrawLists::drawName(uint32_t region) -> const char* {
    // Every view's four regions are consecutive, in this order (regionName).
    static constexpr std::array<const char*, 4> names = {
        "Opaque",
        "Opaque two-sided",
        "Cutout",
        "Cutout two-sided",
    };
    return names[region % names.size()];
}

auto DrawLists::regionName(uint32_t region) -> const char* {
    static constexpr std::array<const char*, 24> names = {
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
        "camera.late.single",
        "camera.late.double",
        "camera.late.single.cutout",
        "camera.late.double.cutout",
    };
    static_assert(names.size() == regionCount);
    return region < names.size() ? names[region] : "region.invalid";
}
