#pragma once

#include "scenehandles.h"
#include "scenetypes.h"

#include <unordered_map>
#include <vector>

struct RenderMeshInstance {
    MeshHandle mesh;
    MaterialHandle material;
    glm::mat4 worldTransform = glm::mat4(1.0f);
    AABB worldBounds;
    // A prim's mesh expands to one instance per material submesh, all sharing
    // the same mesh buffers and transform. [indexOffset, indexOffset+indexCount)
    // is this submesh's range in the mesh index buffer; primFirst marks the
    // first instance of the prim so the shadow pass can draw the whole mesh once.
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    bool primFirst = true;
};

enum class LightType : uint8_t {
    Directional,
    Point,
    Spot,
};

struct RenderLight {
    LightType type = LightType::Directional;
    glm::vec3 color = glm::vec3(1.0f);
    float intensity = 1.0f;
    float exposure = 0.0f; // stops; final radiance = color * intensity * 2^exposure
    float angle = 0.53f;   // distant-light solid angle in degrees
    bool shadowEnable = true;
    glm::vec3 shadowColor = glm::vec3(0.0f);
    glm::mat4 worldTransform = glm::mat4(1.0f);
    PrimHandle primHandle; // identifies the source prim for incremental updates
};

// Contiguous run of meshInstances produced by one prim (one per submesh).
struct InstanceRange {
    uint32_t first = 0;
    uint32_t count = 0;
};

struct RenderWorld {
    std::vector<RenderMeshInstance> meshInstances;
    std::vector<RenderLight> lights;
    // Reverse lookup for incremental transform patching: prim.index -> the run
    // of meshInstances that prim expanded into.
    std::unordered_map<uint32_t, InstanceRange> primToInstance;

    void clear() {
        meshInstances.clear();
        lights.clear();
        primToInstance.clear();
    }
};
