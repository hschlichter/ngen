#pragma once

#include "framegraph.h"
#include "geometrypass.h" // GpuInstance, CachedMesh
#include "rhitypes.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <unordered_map>

class RhiDevice;

struct ShadowPassData {
    FgTextureHandle shadowMap;
};

class ShadowPass {
public:
    auto init(RhiDevice* device, RhiExtent2D extent, RhiFormat depthFormat) -> bool;
    auto destroy(RhiDevice* device) -> void;

    auto addPass(FrameGraph& fg,
                 RhiExtent2D extent,
                 RhiFormat depthFormat,
                 const glm::mat4& lightViewProj,
                 std::span<const GpuInstance> instances,
                 const std::unordered_map<uint32_t, CachedMesh>& meshCache) -> const ShadowPassData&;

private:
    RhiPipeline* pipeline = nullptr;
    RhiShaderModule* vertShader = nullptr;
    RhiShaderModule* fragShader = nullptr;
};
