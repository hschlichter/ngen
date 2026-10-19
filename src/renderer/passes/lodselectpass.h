#pragma once

#include "framegraph.h"
#include "lodgroups.h"
#include "pipelineregistry.h"
#include "rhitypes.h"

#include <cstdint>
#include <vector>

class DeletionQueue;
class RhiDevice;

// Render LOD's level selection (src/renderer/README.md, "Render LOD"): two compute passes running shaders/lodselect.comp in its select and
// sum modes, before the culling passes, which draw each group's instances of the level it picked.
class LodSelectPass {
public:
    auto init(RhiDevice* device, PipelineRegistry& pipelines) -> bool;
    auto destroy(RhiDevice* device) -> void;

    // Rewrites every frame slot's descriptor set; call whenever LodGroups replaced a buffer. The old sets go through the deletion queue.
    auto rebuildDescriptors(RhiDevice* device, const LodGroups& groups, DeletionQueue& deletionQueue, uint64_t frame) -> void;

    // The camera the heuristic measures from: its position in world metres and the view window's area at depth 1.
    struct Camera {
        glm::vec3 eye = glm::vec3(0.0f);
        float windowArea = 1.0f;
    };
    // Adds LodSelect and LodSelectSum for the frame slot; nothing when there are no groups.
    auto addPasses(FrameGraph& fg, const LodGroups::Handles& handles, const Camera& camera, uint32_t groupCount, uint32_t frameSlot) -> void;

    // Whether its pipeline has been created; until then the frame leaves the lit path out.
    auto ready() -> bool;

private:
    PipelineRegistry* registry = nullptr;
    RhiDescriptorSetLayout* setLayout = nullptr;
    PipelineHandle pipelineHandle;
    RhiDescriptorPool* pool = nullptr;
    std::vector<RhiDescriptorSet*> sets; // per frame slot
};
