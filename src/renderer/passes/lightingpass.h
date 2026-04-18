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
    glm::vec4 lightDirection; // xyz = direction toward light, w = unused
    glm::vec4 lightColor;     // xyz = radiance (color * intensity * 2^exposure), w = ambient
    glm::vec4 depthParams;    // x = near, y = far, zw = unused
    glm::vec4 shadowTint;     // xyz = light contribution when shadowed (from UsdLuxShadowAPI::shadow:color), w = unused
    glm::mat4 invViewProj;    // inverse(proj * view), for world-pos reconstruction from gbuffer
    glm::mat4 lightViewProj;  // shadow camera view-projection
};

// Pre-resolved per-frame lighting inputs. The renderer picks one active light and flattens
// it into this small struct so LightingPass doesn't have to re-run the selection logic or
// deal with RenderLight's worldTransform.
struct LightingInputs {
    glm::vec3 direction = glm::vec3(0.0f, 1.0f, 0.0f); // unit vector pointing toward the light
    glm::vec3 radiance = glm::vec3(1.0f);              // color × intensity × 2^exposure, pre-scaled
    glm::vec3 shadowColor = glm::vec3(0.0f);           // attenuation tint from UsdLuxShadowAPI::shadow:color
};

enum class GBufferView : int {
    Lit = 0,
    Albedo,
    Normals,
    Depth,
    ShadowFactor,
    ShadowMap,
    ShadowUV,
    WorldPos,
};

struct LightingPassData {
    FgTextureHandle albedo;
    FgTextureHandle normal;
    FgTextureHandle depth;
    FgTextureHandle shadowMap;
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
                 const LightingInputs& lightInputs,
                 GBufferView viewMode,
                 bool showOverlay,
                 bool showShadowOverlay,
                 const glm::mat4& invViewProj,
                 const glm::mat4& lightViewProj) -> const LightingPassData&;

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
