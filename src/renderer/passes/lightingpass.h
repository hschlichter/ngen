#pragma once

#include "framegraph.h"
#include "pipelineregistry.h"
#include "rhitypes.h"
#include "shadowcascades.h"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <vector>

class RhiDevice;
struct RenderLight;
struct GeometryPassData;

struct LightingUBO {
    glm::vec4 lightDirection;                     // xyz = direction toward light, w = unused
    glm::vec4 lightColor;                         // xyz = radiance (color * intensity * 2^exposure), w = unused
    glm::vec4 depthParams;                        // x = the camera's near plane, y = the depth preview's far end, zw = unused
    glm::vec4 shadowTint;                         // xyz = light contribution when shadowed (from UsdLuxShadowAPI::shadow:color), w = unused
    glm::mat4 invViewProj;                        // inverse(proj * view), for world-pos reconstruction from gbuffer
    glm::mat4 cascadeViewProj[maxShadowCascades]; // light view-projection per cascade
    glm::vec4 cascadeRects[maxShadowCascades];    // atlas tile: xy offset, zw scale
    glm::vec4 cascadeSplits;                      // view-depth end of each cascade
    glm::vec4 cascadeTexelDepth;                  // ndc depth per texel, per cascade; bias unit
    glm::vec4 cascadeTexelWorld;                  // world size of one texel, per cascade; normal offset
    glm::vec4 cascadeParams;                      // x = count, y = pcf (0/1), z = atlas size in texels
    glm::vec4 ambientUp;                          // xyz = the scene's up axis, w = unused
    glm::vec4 skyAmbient;                         // xyz = ambient on faces turned up, as a fraction of the light's radiance
    glm::vec4 groundAmbient;                      // xyz = ambient on faces turned down, as a fraction of the light's radiance
};

// Pre-resolved per-frame lighting inputs. The renderer picks one active light and flattens
// it into this small struct so LightingPass doesn't have to re-run the selection logic or
// deal with RenderLight's worldTransform.
// The engine's ambient until scenes light it themselves (dome lights): a sky colour on faces turned up and a ground colour on faces turned
// down, blended by how much a face turns up, as fractions of the sun's radiance. The defaults are a slightly blue sky and a darker, warm
// ground; a vertical face gets their average, about the constant 0.15 the view had before.
struct AmbientSettings {
    glm::vec3 sky = glm::vec3(0.20f, 0.21f, 0.24f);
    glm::vec3 ground = glm::vec3(0.10f, 0.09f, 0.08f);

    auto operator==(const AmbientSettings&) const -> bool = default;
};

struct LightingInputs {
    glm::vec3 direction = glm::vec3(0.0f, 1.0f, 0.0f); // unit vector pointing toward the light
    glm::vec3 radiance = glm::vec3(1.0f);              // color × intensity × 2^exposure, pre-scaled
    glm::vec3 shadowColor = glm::vec3(0.0f);           // attenuation tint from UsdLuxShadowAPI::shadow:color
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);        // the scene's up axis, for the hemisphere ambient
    AmbientSettings ambient;
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
    MipLevel,
    Cascades,
    LodLevel,
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
    auto init(RhiDevice* device, PipelineRegistry& pipelines, uint32_t imageCount, RhiExtent2D extent, RhiFormat colorFormat) -> bool;
    auto destroy(RhiDevice* device) -> void;

    auto addPass(
        FrameGraph& fg,
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
        RhiSampler* shadowSampler,
        std::span<const ShadowCascade> cascades,
        bool pcf,
        RhiExtent2D atlasExtent,
        float previewFar) -> const LightingPassData&;

    // Whether its pipelines have been created; until then the frame leaves the pass out.
    auto ready() -> bool;

private:
    RhiDevice* device = nullptr;
    PipelineRegistry* registry = nullptr;
    PipelineHandle pipelineHandle;
    RhiDescriptorSetLayout* descriptorSetLayout = nullptr;
    RhiDescriptorPool* descriptorPool = nullptr;
    std::vector<RhiDescriptorSet*> descriptorSets;
    std::vector<RhiBuffer*> uniformBuffers;
    std::vector<void*> uniformBuffersMapped;
    RhiFormat sceneColorFormat = RhiFormat::Undefined;
};
