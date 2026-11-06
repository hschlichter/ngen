#pragma once

#include "drawlists.h"
#include "framegraph.h"
#include "geometrypass.h" // GpuInstance, GpuScene
#include "pipelineregistry.h"
#include "rhitypes.h"
#include "shadowcascades.h"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <unordered_map>

class RhiDevice;
class DeletionQueue;

struct ShadowPassData {
    FgTextureHandle shadowMap;
};

class ShadowPass {
public:
    // Every pipeline uses the geometry pass's set layout: the scene tables and the geometry pool the task and mesh shaders read.
    auto init(PipelineRegistry& pipelines, RhiFormat depthFormat, RhiDescriptorSetLayout* geometrySetLayout) -> bool;

    // One atlas texture of atlasExtent; each cascade draws into its tile with its own
    // viewport, scissor and light matrix. Cascade c draws DrawLists::cascadeRegion(c, *)
    // indirectly, its meshlets culled against the cascade (meshlet.task); its cutout buckets discard where the opacity is below the
    // threshold (shadowcutout.mesh, cutout.frag).
    auto addPass(
        FrameGraph& fg,
        RhiExtent2D atlasExtent,
        RhiFormat depthFormat,
        std::span<const ShadowCascade> cascades,
        std::span<const GpuInstance> instances,
        FgBufferHandle instanceBuffer,
        const DrawLists& lists,
        DrawLists::Handles drawHandles,
        const GpuScene& scene,
        RhiDescriptorSet* geometrySet,
        FgTextureHandle hzb) -> const ShadowPassData&;

    // Whether its pipelines have been created; until then the frame leaves the pass out.
    auto ready() -> bool;

private:
    PipelineRegistry* registry = nullptr;
    // By bucket (DrawLists::bucket): single-sided, double-sided, then the same two cutout.
    std::array<PipelineHandle, DrawLists::bucketCount> pipelines = {};
};
