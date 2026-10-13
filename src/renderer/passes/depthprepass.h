#pragma once

#include "drawlists.h"
#include "framegraph.h"
#include "geometrypass.h" // GpuInstance, GpuScene
#include "pipelineregistry.h"
#include "rhitypes.h"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <unordered_map>

class RhiDevice;

struct DepthPrepassData {
    FgTextureHandle depth;
};

// Depth-only pass over the visible instances before the geometry pass, so the geometry
// pass can run with an Equal depth test and shade each pixel once.
// depthonly.vert mirrors gbuffer.vert's position
// expression with gl_Position invariant, so the Equal test holds; no fragment work. Cutout
// buckets draw from the full vertex stream (depthcutout.vert, the same position expression)
// and discard where the material's opacity is below its threshold (cutout.frag).
class DepthPrepass {
public:
    // Uses the geometry pass's descriptor set layout: the UBO at binding 0 and the instance
    // buffer at binding 2 are what the position depends on; the cutout pipelines also read the
    // textures (binding 1) and the material table (binding 3).
    auto init(PipelineRegistry& pipelines, RhiFormat depthFormat, RhiDescriptorSetLayout* geometrySetLayout) -> bool;

    auto addPass(
        FrameGraph& fg,
        FgTextureHandle depthHandle,
        RhiExtent2D extent,
        std::span<const GpuInstance> instances,
        FgBufferHandle instanceBuffer,
        const DrawLists& lists,
        DrawLists::Handles drawHandles,
        const GpuScene& scene,
        RhiDescriptorSet* descriptorSet) -> const DepthPrepassData&;

    // Whether its pipelines have been created; until then the frame leaves the pass out.
    auto ready() -> bool;

private:
    PipelineRegistry* registry = nullptr;
    // By bucket (DrawLists::bucket): single-sided, double-sided, then the same two cutout.
    std::array<PipelineHandle, DrawLists::bucketCount> pipelines = {};
};
