#pragma once

#include "drawlists.h"
#include "framegraph.h"
#include "lodgroups.h"
#include "pipelineregistry.h"
#include "rhitypes.h"

#include <array>
#include <cstdint>
#include <vector>

class DeletionQueue;
class DepthPyramid;
class GpuScene;
class RhiDevice;

// GPU instance culling (src/renderer/README.md, "Culling and draw lists"): compute passes running
// shaders/instancecull.comp in its cull, scan and scatter modes, and with occlusion its second phase (occlusion, scan, scatter) after
// the depth pyramid is built. Writes the draw commands and counts the shadow, prepass and geometry passes read indirectly.
class InstanceCullPass {
public:
    auto init(RhiDevice* device, PipelineRegistry& pipelines) -> bool;
    auto destroy(RhiDevice* device) -> void;

    // Rewrites every frame slot's descriptor set. Call whenever the instance buffer, the
    // mesh table, the draw-list buffers or the render-LOD buffers are replaced; the old sets go
    // through the deletion queue at `frame`.
    auto rebuildDescriptors(RhiDevice* device, const DrawLists& lists, const GpuScene& scene, const LodGroups& lodGroups, const DepthPyramid& pyramid, DeletionQueue& deletionQueue, uint64_t frame)
        -> void;

    // Adds InstanceCull, InstanceCullScan and InstanceCullScatter for the frame slot (CullHistoryClear first when the history is new);
    // the cull pass reads the levels LodSelect picked and marks levels whose meshes aren't in the pool. Every pass binds the depth pyramid
    // (`hzb`), so each reads it.
    auto addPasses(FrameGraph& fg, DrawLists& lists, const DrawLists::Handles& handles, const LodGroups::Handles& lodHandles, FgBufferHandle instanceBuffer, FgTextureHandle hzb, uint32_t groupCount, uint32_t frameSlot)
        -> void;
    // The second phase, after HzbBuild: InstanceCullOcclusion, InstanceCullLateScan and InstanceCullLateScatter.
    auto addLatePasses(FrameGraph& fg, const DrawLists::Handles& handles, FgBufferHandle instanceBuffer, FgTextureHandle hzb, uint32_t groupCount, uint32_t frameSlot) -> void;

    // Whether its pipelines have been created; until then the frame leaves the pass out.
    auto ready() -> bool;

private:
    PipelineRegistry* registry = nullptr;
    RhiDescriptorSetLayout* setLayout = nullptr;
    PipelineHandle pipelineHandle;
    RhiDescriptorPool* pool = nullptr;
    std::vector<RhiDescriptorSet*> sets; // per frame slot
};
