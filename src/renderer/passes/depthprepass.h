#pragma once

#include "framegraph.h"
#include "geometrypass.h" // GpuInstance, CachedMesh
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
    // Uses the geometry pass's descriptor set layout: the UBO at binding 0 is the one
    // the position depends on; the texture binding is unused.
    auto init(RhiDevice* device, RhiFormat depthFormat, RhiDescriptorSetLayout* geometrySetLayout) -> bool;
    auto destroy(RhiDevice* device) -> void;

    auto addPass(
        FrameGraph& fg,
        FgTextureHandle depthHandle,
        RhiExtent2D extent,
        uint32_t imageIndex,
        std::span<const GpuInstance> instances,
        std::span<const uint8_t> visible,
        const std::unordered_map<uint32_t, CachedMesh>& meshCache,
        std::span<RhiDescriptorSet*> descriptorSets) -> const DepthPrepassData&;

private:
    RhiPipeline* pipelineCullBack = nullptr;
    RhiPipeline* pipelineCullNone = nullptr;
    RhiShaderModule* vertShader = nullptr;
    RhiShaderModule* fragShader = nullptr;
};
