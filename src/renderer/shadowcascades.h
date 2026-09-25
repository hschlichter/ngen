#pragma once

#include "renderworld.h"
#include "rhitypes.h"
#include "scenetypes.h"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>

// Cascaded shadow maps (docs/plan_shadow_cascades.md): the camera frustum is split along
// view depth, each slice gets its own ortho light frustum fitted to the slice's bounding
// sphere and snapped to texel increments, and all cascades share one atlas texture as
// tiles. Fitted on the main thread next to the camera cull; the render thread receives
// the matrices and visibility masks in the snapshot.
constexpr uint32_t maxShadowCascades = 4;

struct ShadowCascadeSettings {
    uint32_t count = 3;       // 1..maxShadowCascades; three by default, the fourth tile stays free
    uint32_t tileSize = 1024; // texels per cascade
    float splitLambda = 0.5f; // 0 = uniform splits, 1 = logarithmic
    bool pcf = true;          // hardware compare sampler with a 3x3 tap loop; off = one manual compare

    auto operator==(const ShadowCascadeSettings&) const -> bool = default;
};

struct ShadowCascade {
    glm::mat4 viewProj = glm::mat4(1.0f); // light view-projection for this tile
    float splitFar = 0.0f;                // view-space depth where the cascade ends
    float texelWorldSize = 0.0f;          // world units per shadow texel
    float texelDepthNdc = 0.0f;           // ndc depth change over one texel's world size; bias unit
    glm::vec4 atlasRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f); // xy offset, zw scale in atlas UV
};

// Atlas size for the settings: one tile, or a 2x2 of tiles for more than one cascade.
auto shadowAtlasExtent(const ShadowCascadeSettings& settings) -> RhiExtent2D;

// The renderer's light pick, on the main thread: first directional light with shadows
// enabled, else the first directional light, else the world up. Returns the direction
// toward the light.
auto pickShadowLightDirection(std::span<const RenderLight> lights, glm::vec3 worldUp) -> glm::vec3;

// Union of the instance world bounds; invalid when there are none.
auto sceneBoundsOf(std::span<const RenderMeshInstance> instances) -> AABB;

// Fits the cascades. nearZ/farZ are the camera's; farZ is pulled in to the scene bounds so
// no cascade is spent behind the last mesh. Returns the cascade count written to out.
auto fitShadowCascades(const ShadowCascadeSettings& settings, const glm::mat4& view, const glm::mat4& proj, float nearZ, float farZ, glm::vec3 lightDir, glm::vec3 worldUp, const AABB& sceneBounds, std::span<ShadowCascade> out) -> uint32_t;
