#pragma once

#include "drawlists.h"
#include "framegraph.h"
#include "geometrypass.h" // GpuInstance, GpuScene
#include "rhitypes.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <unordered_map>

class RhiDevice;

struct DepthPrepassData {
    FgTextureHandle depth;
};

// Depth-only pass over the visible instances before the geometry pass, so the geometry
// pass can run with an Equal depth test and shade each pixel once
// (docs/plan_geometry_pass_cost.md). depthonly.vert mirrors gbuffer.vert's position
// expression with gl_Position invariant, so the Equal test holds; no fragment work.
class DepthPrepass {
public:
    // Uses the geometry pass's descriptor set layout: the UBO at binding 0 and the instance
    // buffer at binding 2 are what the position depends on; the texture binding is unused.
    auto init(RhiDevice* device, RhiFormat depthFormat, RhiDescriptorSetLayout* geometrySetLayout) -> bool;
    auto destroy(RhiDevice* device) -> void;

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

private:
    RhiPipeline* pipelineCullBack = nullptr;
    RhiPipeline* pipelineCullNone = nullptr;
    RhiShaderModule* vertShader = nullptr;
    RhiShaderModule* fragShader = nullptr;
};
