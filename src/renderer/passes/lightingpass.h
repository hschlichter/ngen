#pragma once

#include "framegraph.h"
#include "rhitypes.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

class RhiDevice;
struct RenderWorld;
struct GeometryPassData;

struct LightingUBO {
    glm::vec4 lightDirection; // xyz = direction, w = intensity
    glm::vec4 lightColor;     // xyz = color, w = ambient
    glm::vec4 depthParams;    // x = near, y = far, zw = unused
};

enum class GBufferView : int {
    Lit = 0,
    Albedo,
    Normals,
    Depth,
};

class LightingPass {
public:
    auto init(RhiDevice* device, uint32_t imageCount, RhiExtent2D extent, RhiFormat colorFormat) -> bool;
    auto destroy(RhiDevice* device) -> void;

    auto addPass(FrameGraph& fg,
                 const GeometryPassData& geomData,
                 FgTextureHandle depthHandle,
                 FgTextureHandle colorHandle,
                 RhiExtent2D extent,
                 uint32_t imageIndex,
                 RhiSampler* sampler,
                 const RenderWorld& world,
                 GBufferView viewMode,
                 bool showOverlay) -> void;

private:
    RhiDevice* device = nullptr;
    RhiPipeline* pipeline = nullptr;
    RhiDescriptorSetLayout* descriptorSetLayout = nullptr;
    RhiDescriptorPool* descriptorPool = nullptr;
    std::vector<RhiDescriptorSet*> descriptorSets;
    RhiShaderModule* vertShader = nullptr;
    RhiShaderModule* fragShader = nullptr;
    std::vector<RhiBuffer*> uniformBuffers;
    std::vector<void*> uniformBuffersMapped;
};
