#include "instancecullpass.h"

#include "deletionqueue.h"
#include "gpuscene.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "shaderloader.h"

#include <array>
#include <format>

namespace {

enum class CullMode : uint32_t {
    Cull = 0,
    Scan = 1,
    Scatter = 2,
};

constexpr std::array<RhiDescriptorBinding, 9> cullBindings = {{
    {.binding = 0, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}, // params
    {.binding = 1, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}, // instances
    {.binding = 2, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}, // mesh table
    {.binding = 3, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}, // visibility
    {.binding = 4, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}, // group counters
    {.binding = 5, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}, // group offsets
    {.binding = 6, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}, // commands
    {.binding = 7, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}, // totals / counts
    {.binding = 8, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}, // cull reason per view
}};

struct CullPassData {
    FgBufferHandle visibility;
    FgBufferHandle cullPlanes;
    FgBufferHandle groupCounters;
};

struct ScanPassData {
    FgBufferHandle groupOffsets;
    FgBufferHandle counts;
};

struct ScatterPassData {
    FgBufferHandle commands;
};

} // namespace

auto InstanceCullPass::init(RhiDevice* device) -> bool {
    shader = loadShaderModule(device, RhiShaderStage::Compute, "shaders/instancecull.comp.spv");
    if (shader == nullptr) {
        return false;
    }
    setLayout = device->createDescriptorSetLayout(cullBindings);
    device->setDebugName(setLayout, "cull.setlayout");
    RhiComputePipelineDesc pipelineDesc = {
        .shader = shader,
        .descriptorSetLayouts = {&setLayout, 1},
        .pushConstant = {.stage = RhiShaderStage::Compute, .offset = 0, .size = sizeof(uint32_t)},
    };
    pipeline = device->createComputePipeline(pipelineDesc);
    device->setDebugName(pipeline, "cull.pipeline");
    return pipeline != nullptr;
}

auto InstanceCullPass::destroy(RhiDevice* device) -> void {
    if (pool != nullptr) {
        device->freeDescriptorSets(pool, sets);
        device->destroyDescriptorPool(pool);
    }
    device->destroyPipeline(pipeline);
    device->destroyDescriptorSetLayout(setLayout);
    device->destroyShaderModule(shader);
}

auto InstanceCullPass::rebuildDescriptors(RhiDevice* device, const DrawLists& lists, const GpuScene& scene, DeletionQueue& deletionQueue, uint64_t frame) -> void {
    if (pool != nullptr) {
        deletionQueue.defer(frame, [device, oldPool = pool, oldSets = sets] {
            device->freeDescriptorSets(oldPool, oldSets);
            device->destroyDescriptorPool(oldPool);
        });
        pool = nullptr;
        sets.clear();
    }
    if (scene.instanceBuffer() == nullptr || scene.meshTableBuffer() == nullptr) {
        return;
    }
    auto slotCount = lists.frameSlots();
    pool = device->createDescriptorPool(slotCount, cullBindings);
    device->setDebugName(pool, "cull.sets.pool");
    sets.assign(slotCount, nullptr);
    device->allocateDescriptorSets(pool, setLayout, sets);
    for (uint32_t i = 0; i < slotCount; i++) {
        device->setDebugName(sets[i], std::format("cull.set.slot{}", i).c_str());
    }
    for (uint32_t i = 0; i < slotCount; i++) {
        auto buffers = lists.slotBuffers(i);
        std::array<RhiDescriptorWrite, 9> writes = {{
            {.binding = 0, .type = RhiDescriptorType::StorageBuffer, .buffer = buffers.params},
            {.binding = 1, .type = RhiDescriptorType::StorageBuffer, .buffer = scene.instanceBuffer()},
            {.binding = 2, .type = RhiDescriptorType::StorageBuffer, .buffer = scene.meshTableBuffer()},
            {.binding = 3, .type = RhiDescriptorType::StorageBuffer, .buffer = buffers.visibility},
            {.binding = 4, .type = RhiDescriptorType::StorageBuffer, .buffer = buffers.groupCounters},
            {.binding = 5, .type = RhiDescriptorType::StorageBuffer, .buffer = buffers.groupOffsets},
            {.binding = 6, .type = RhiDescriptorType::StorageBuffer, .buffer = buffers.commands},
            {.binding = 7, .type = RhiDescriptorType::StorageBuffer, .buffer = buffers.counts},
            {.binding = 8, .type = RhiDescriptorType::StorageBuffer, .buffer = buffers.cullPlanes},
        }};
        device->updateDescriptorSet(sets[i], writes);
    }
}

auto InstanceCullPass::addPasses(FrameGraph& fg, const DrawLists::Handles& handles, FgBufferHandle instanceBuffer, uint32_t groupCount, uint32_t frameSlot) -> void {
    auto* pip = pipeline;
    auto* set = frameSlot < sets.size() ? sets[frameSlot] : nullptr;
    auto dispatchMode = [pip, set](FrameGraphContext& ctx, CullMode mode, uint32_t groups) {
        auto* cmd = ctx.cmd();
        cmd->bindPipeline(pip);
        cmd->bindDescriptorSet(pip, 0, set);
        auto value = (uint32_t) mode;
        cmd->pushConstants(pip, RhiShaderStage::Compute, 0, sizeof(value), &value);
        cmd->dispatch(groups, 1, 1);
    };

    // 1. Frustum test per instance and view; per-workgroup counters.
    fg.addPass<CullPassData>(
        "InstanceCull",
        [&](FrameGraphBuilder& builder, CullPassData& data) {
            builder.read(handles.params, FgAccessFlags::StorageRead);
            builder.read(instanceBuffer, FgAccessFlags::StorageRead);
            data.visibility = builder.write(handles.visibility, FgAccessFlags::StorageWrite);
            data.cullPlanes = builder.write(handles.cullPlanes, FgAccessFlags::StorageWrite);
            data.groupCounters = builder.write(handles.groupCounters, FgAccessFlags::StorageWrite);
        },
        [dispatchMode, groupCount](FrameGraphContext& ctx, const CullPassData&) { dispatchMode(ctx, CullMode::Cull, groupCount); });

    // 2. Exclusive prefix over the workgroups: start offsets, draw counts and statistics.
    fg.addPass<ScanPassData>(
        "InstanceCullScan",
        [&](FrameGraphBuilder& builder, ScanPassData& data) {
            builder.read(handles.params, FgAccessFlags::StorageRead);
            builder.read(handles.groupCounters, FgAccessFlags::StorageRead);
            data.groupOffsets = builder.write(handles.groupOffsets, FgAccessFlags::StorageWrite);
            data.counts = builder.write(handles.counts, FgAccessFlags::StorageWrite);
        },
        [dispatchMode](FrameGraphContext& ctx, const ScanPassData&) { dispatchMode(ctx, CullMode::Scan, 1); });

    // 3. Ordered writes: each command lands at its workgroup's offset plus the local prefix.
    fg.addPass<ScatterPassData>(
        "InstanceCullScatter",
        [&](FrameGraphBuilder& builder, ScatterPassData& data) {
            builder.read(handles.params, FgAccessFlags::StorageRead);
            builder.read(instanceBuffer, FgAccessFlags::StorageRead);
            builder.read(handles.visibility, FgAccessFlags::StorageRead);
            builder.read(handles.groupOffsets, FgAccessFlags::StorageRead);
            data.commands = builder.write(handles.commands, FgAccessFlags::StorageWrite);
        },
        [dispatchMode, groupCount](FrameGraphContext& ctx, const ScatterPassData&) { dispatchMode(ctx, CullMode::Scatter, groupCount); });
}
