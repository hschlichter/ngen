#pragma once

#include "framegraph.h"
#include "rhitypes.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

class RhiDevice;
struct RenderLight;
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

struct LightingPassData {
    FgTextureHandle albedo;
    FgTextureHandle normal;
    FgTextureHandle depth;
    FgTextureHandle sceneColor;
};

class LightingPass {
public:
    auto init(RhiDevice* device, uint32_t imageCount, RhiExtent2D extent, RhiFormat colorFormat) -> bool;
    auto destroy(RhiDevice* device) -> void;

    auto addPass(FrameGraph& fg,
                 const GeometryPassData& geomData,
                 FgTextureHandle depthHandle,
                 FgTextureHandle shadowHandle,
                 RhiExtent2D extent,
                 uint32_t imageIndex,
                 RhiSampler* sampler,
                 const std::vector<RenderLight>& lights,
                 GBufferView viewMode,
                 bool showOverlay) -> const LightingPassData&;

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
    RhiFormat sceneColorFormat = RhiFormat::Undefined;
};
