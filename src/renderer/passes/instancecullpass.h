#pragma once

#include "drawlists.h"
#include "framegraph.h"
#include "pipelineregistry.h"
#include "rhitypes.h"

#include <array>
#include <cstdint>
#include <vector>

class DeletionQueue;
class GpuScene;
class RhiDevice;

// GPU instance culling (src/renderer/README.md, "GPU-driven frame"): three compute passes running
// shaders/instancecull.comp in its cull, scan and scatter modes. Writes the draw commands
// and counts the shadow, prepass and geometry passes read indirectly.
class InstanceCullPass {
public:
    auto init(RhiDevice* device, PipelineRegistry& pipelines) -> bool;
    auto destroy(RhiDevice* device) -> void;

    // Rewrites every frame slot's descriptor set. Call whenever the instance buffer, the
    // mesh table or the draw-list buffers are replaced; the old sets go through the deletion
    // queue at `frame`.
    auto rebuildDescriptors(RhiDevice* device, const DrawLists& lists, const GpuScene& scene, DeletionQueue& deletionQueue, uint64_t frame) -> void;

    // Adds InstanceCull, InstanceCullScan and InstanceCullScatter for the frame slot.
    auto addPasses(FrameGraph& fg, const DrawLists::Handles& handles, FgBufferHandle instanceBuffer, uint32_t groupCount, uint32_t frameSlot) -> void;

    // Whether its pipelines have been created; until then the frame leaves the pass out.
    auto ready() -> bool;

private:
    PipelineRegistry* registry = nullptr;
    RhiDescriptorSetLayout* setLayout = nullptr;
    PipelineHandle pipelineHandle;
    RhiDescriptorPool* pool = nullptr;
    std::vector<RhiDescriptorSet*> sets; // per frame slot
};
