#pragma once

#include "renderworld.h"
#include "rhitypes.h"
#include "scenetypes.h"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>

// Cascaded shadow maps: the camera frustum is split along
// view depth, each slice gets its own ortho light frustum fitted to the slice's bounding
// sphere and snapped to texel increments, and all cascades share one atlas texture as
// tiles. Fitted on the main thread; the render thread receives the matrices in the
// snapshot, and the GPU culling tests instances against each cascade.
constexpr uint32_t maxShadowCascades = 4;

struct ShadowCascadeSettings {
    uint32_t count = 3;         // 1..maxShadowCascades; three by default, the fourth tile stays free
    uint32_t tileSize = 1024;   // texels per cascade
    float splitLambda = 0.5f;   // 0 = uniform splits, 1 = logarithmic
    bool pcf = true;            // hardware compare sampler with a 3x3 tap loop; off = one manual compare
    float maxDistance = 200.0f; // metres from the camera where shadows end; beyond, surfaces are lit without shadow

    auto operator==(const ShadowCascadeSettings&) const -> bool = default;
};

struct ShadowCascade {
    glm::mat4 viewProj = glm::mat4(1.0f);                    // light view-projection for this tile
    float splitFar = 0.0f;                                   // view-space depth where the cascade ends
    float texelWorldSize = 0.0f;                             // world units per shadow texel
    float texelDepthNdc = 0.0f;                              // ndc depth change over one texel's world size; bias unit
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

// The union of every instance's world bounds, kept per chunk of the instance list: a chunk is read again only when the list holds another
// chunk in its place (a write copies a chunk this cache holds), so a frame where nothing changed reads no instance.
class SceneBoundsCache {
public:
    auto update(const ChunkedList<RenderMeshInstance>& instances) -> AABB;
    // The bounds of a chunk of the instances (ChunkedList::chunk) as of the last update, invalid for a chunk with nothing alive; the ground
    // test skips a chunk by them.
    auto chunkCount() const -> size_t { return chunks.size(); }
    auto chunkBounds(size_t index) const -> const AABB& { return chunks[index].bounds; }

private:
    struct Chunk {
        std::shared_ptr<const std::vector<RenderMeshInstance>> data;
        AABB bounds;
    };
    std::vector<Chunk> chunks;
};

// Fits the cascades from the camera's near plane to the shadow distance (settings.maxDistance), pulled in to the scene bounds so no
// cascade is spent behind the last mesh. The camera's projection has no far plane. Returns the cascade count written to out.
auto fitShadowCascades(const ShadowCascadeSettings& settings, const glm::mat4& view, const glm::mat4& proj, float nearZ, glm::vec3 lightDir, glm::vec3 worldUp, const AABB& sceneBounds, std::span<ShadowCascade> out) -> uint32_t;
