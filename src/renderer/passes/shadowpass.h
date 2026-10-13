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
    // The cutout pipelines use the geometry pass's set layout: instances, material table and textures.
    auto init(RhiDevice* device, PipelineRegistry& pipelines, RhiExtent2D extent, RhiFormat depthFormat, RhiDescriptorSetLayout* geometrySetLayout) -> bool;
    auto destroy(RhiDevice* device) -> void;

    // Points the pass's descriptor set at the instance buffer. Called whenever the buffer is
    // (re)created; the old set may still be bound by frames in flight, so it is freed through
    // the deletion queue at `frame`.
    auto bindInstanceBuffer(RhiDevice* device, RhiBuffer* instanceBuffer, DeletionQueue& deletionQueue, uint64_t frame) -> void;

    // One atlas texture of atlasExtent; each cascade draws into its tile with its own
    // viewport, scissor and light matrix. Cascade c draws DrawLists::cascadeRegion(c, *)
    // indirectly; its cutout buckets discard where the opacity is below the threshold
    // (shadowcutout.vert, cutout.frag) and bind the geometry set.
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
        RhiDescriptorSet* geometrySet) -> const ShadowPassData&;

    // Whether its pipelines have been created; until then the frame leaves the pass out.
    auto ready() -> bool;

private:
    RhiDescriptorSetLayout* descSetLayout = nullptr; // binding 0: instance buffer
    RhiDescriptorPool* descPool = nullptr;
    RhiDescriptorSet* descSet = nullptr;
    PipelineRegistry* registry = nullptr;
    // By bucket (DrawLists::bucket): single-sided, double-sided, then the same two cutout.
    std::array<PipelineHandle, DrawLists::bucketCount> pipelines = {};
};
