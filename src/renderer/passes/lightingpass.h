#pragma once

#include "framegraph.h"
#include "rhitypes.h"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

class RhiDevice;
struct RenderLight;
struct GeometryPassData;

// Maximum lights uploaded per frame. Shared with lighting.frag (MAX_LIGHTS there
// must match). Excess scene lights are dropped; keep in sync if you raise it.
inline constexpr int kMaxLights = 16;

// One light, packed for std140. Matches `GpuLight` in lighting.frag.
struct GpuLight {
    glm::vec4 posOrDir; // xyz = world position (point) or direction-toward-light (directional); w = type (0 = dir, 1 = point)
    glm::vec4 radiance; // rgb = color * intensity * 2^exposure; a = range (0 = no cutoff)
};

struct LightingUBO {
    glm::vec4 sunDirection;  // xyz = direction toward the shadow-casting sun (for shadow bias); w = light count
    glm::vec4 ambientParams; // x = ambient level, y = shadow light index (-1 = none), zw = unused
    glm::vec4 depthParams;   // x = near, y = far, zw = unused
    glm::vec4 shadowTint;    // xyz = light contribution when shadowed (from UsdLuxShadowAPI::shadow:color), w = unused
    glm::mat4 invViewProj;   // inverse(proj * view), for world-pos reconstruction from gbuffer
    glm::mat4 lightViewProj; // shadow camera view-projection
    std::array<GpuLight, kMaxLights> lights;
};

// Pre-resolved per-frame lighting inputs. The renderer flattens RenderWorld's lights into
// this so LightingPass doesn't re-run selection or deal with RenderLight's worldTransform.
struct LightingInputs {
    std::vector<GpuLight> lights;                         // resolved scene lights (clamped to kMaxLights)
    glm::vec3 sunDirection = glm::vec3(0.0f, 1.0f, 0.0f); // shadow-caster direction, for the shadow slope bias
    glm::vec3 shadowColor = glm::vec3(0.0f);              // attenuation tint from UsdLuxShadowAPI::shadow:color
    int shadowLightIndex = -1;                            // index into `lights` that casts the shadow map, or -1
    float ambient = 0.15f;                                // flat ambient level
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
