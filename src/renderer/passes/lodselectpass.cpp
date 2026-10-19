#include "lodselectpass.h"

#include "deletionqueue.h"
#include "framegraphcontext.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <array>
#include <format>

namespace {

constexpr std::array<RhiDescriptorBinding, 5> selectBindings = {{
    {.binding = 0, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}, // group table
    {.binding = 1, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}, // states
    {.binding = 2, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}, // residency
    {.binding = 3, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}, // per-workgroup counters
    {.binding = 4, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}, // totals
}};

// Matches Push in lodselect.comp.
struct SelectPush {
    glm::vec3 eye = glm::vec3(0.0f);
    float windowArea = 1.0f;
    uint32_t groupCount = 0;
    uint32_t mode = 0;
    uint32_t workgroupCount = 0;
};

struct SelectPassData {
    FgBufferHandle states;
    FgBufferHandle residency;
    FgBufferHandle groupCounters;
};

struct SumPassData {
    FgBufferHandle totals;
};

} // namespace

auto LodSelectPass::init(RhiDevice* device, PipelineRegistry& pipelines) -> bool {
    registry = &pipelines;
    setLayout = device->createDescriptorSetLayout(selectBindings);
    device->setDebugName(setLayout, "lodselect.setlayout");
    ComputePipelineRequest request = {
        .shader = {.id = "shaders/lodselect.comp", .stage = RhiShaderStage::Compute},
        .descriptorSetLayouts = {setLayout},
        .pushConstant = {.stage = RhiShaderStage::Compute, .offset = 0, .size = sizeof(SelectPush)},
    };
    pipelineHandle = pipelines.registerCompute("lodselect.pipeline", request, PipelineUse::AtStart);
    return true;
}

auto LodSelectPass::destroy(RhiDevice* device) -> void {
    if (pool != nullptr) {
        device->freeDescriptorSets(pool, sets);
        device->destroyDescriptorPool(pool);
    }
    device->destroyDescriptorSetLayout(setLayout);
}

auto LodSelectPass::rebuildDescriptors(RhiDevice* device, const LodGroups& groups, DeletionQueue& deletionQueue, uint64_t frame) -> void {
    if (pool != nullptr) {
        deletionQueue.defer(frame, [device, oldPool = pool, oldSets = sets] {
            device->freeDescriptorSets(oldPool, oldSets);
            device->destroyDescriptorPool(oldPool);
        });
        pool = nullptr;
        sets.clear();
    }
    auto slotCount = groups.frameSlots();
    pool = device->createDescriptorPool(slotCount, selectBindings);
    device->setDebugName(pool, "lodselect.sets.pool");
    sets.assign(slotCount, nullptr);
    device->allocateDescriptorSets(pool, setLayout, sets);
    for (uint32_t i = 0; i < slotCount; i++) {
        device->setDebugName(sets[i], std::format("lodselect.set.slot{}", i).c_str());
        std::array<RhiDescriptorWrite, 5> writes = {{
            {.binding = 0, .type = RhiDescriptorType::StorageBuffer, .buffer = groups.tableBuffer()},
            {.binding = 1, .type = RhiDescriptorType::StorageBuffer, .buffer = groups.stateBuffer()},
            {.binding = 2, .type = RhiDescriptorType::StorageBuffer, .buffer = groups.residencyBuffer()},
            {.binding = 3, .type = RhiDescriptorType::StorageBuffer, .buffer = groups.groupCountersBuffer(i)},
            {.binding = 4, .type = RhiDescriptorType::StorageBuffer, .buffer = groups.totalsBuffer(i)},
        }};
        device->updateDescriptorSet(sets[i], writes);
    }
}

auto LodSelectPass::addPasses(FrameGraph& fg, const LodGroups::Handles& handles, const Camera& camera, uint32_t groupCount, uint32_t frameSlot) -> void {
    if (groupCount == 0) {
        return;
    }
    auto* pip = registry->get(pipelineHandle);
    auto* set = frameSlot < sets.size() ? sets[frameSlot] : nullptr;
    SelectPush push{.eye = camera.eye, .windowArea = camera.windowArea, .groupCount = groupCount, .mode = 0, .workgroupCount = LodGroups::workgroupsFor(groupCount)};
    auto dispatchMode = [pip, set, push](FrameGraphContext& ctx, uint32_t mode, uint32_t workgroups) {
        auto* cmd = ctx.cmd();
        cmd->bindPipeline(pip);
        cmd->bindDescriptorSet(pip, 0, set);
        auto values = push;
        values.mode = mode;
        cmd->pushConstants(pip, RhiShaderStage::Compute, 0, sizeof(values), &values);
        cmd->dispatch(workgroups, 1, 1);
    };

    // 1. The level each group wants and draws; the residency mask cleared; per-workgroup counters.
    fg.addPass<SelectPassData>(
        "LodSelect",
        [&](FrameGraphBuilder& builder, SelectPassData& data) {
            builder.read(handles.table, FgAccessFlags::StorageRead);
            data.states = builder.write(handles.states, FgAccessFlags::StorageWrite);
            data.residency = builder.write(handles.residency, FgAccessFlags::StorageWrite);
            data.groupCounters = builder.write(handles.groupCounters, FgAccessFlags::StorageWrite);
        },
        [dispatchMode, workgroups = push.workgroupCount](FrameGraphContext& ctx, const SelectPassData&) { dispatchMode(ctx, 0, workgroups); });

    // 2. The counters summed in workgroup order into the totals the readback copies.
    fg.addPass<SumPassData>(
        "LodSelectSum",
        [&](FrameGraphBuilder& builder, SumPassData& data) {
            builder.read(handles.groupCounters, FgAccessFlags::StorageRead);
            data.totals = builder.write(handles.totals, FgAccessFlags::StorageWrite);
        },
        [dispatchMode](FrameGraphContext& ctx, const SumPassData&) { dispatchMode(ctx, 1, 1); });
}

auto LodSelectPass::ready() -> bool {
    std::array<PipelineHandle, 1> handles = {pipelineHandle};
    return registry->ready(handles);
}
