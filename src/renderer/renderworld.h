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
    float exposure = 0.0f;         // stops; final radiance = color * intensity * 2^exposure
    float angle = 0.53f;           // distant-light solid angle in degrees
    bool shadowEnable = true;
    glm::vec3 shadowColor = glm::vec3(0.0f);
    glm::mat4 worldTransform = glm::mat4(1.0f);
    PrimHandle primHandle;         // identifies the source prim for incremental updates
};

struct RenderWorld {
    std::vector<RenderMeshInstance> meshInstances;
    std::vector<RenderLight> lights;
    // Reverse lookup for incremental transform patching: prim.index -> meshInstances index.
    std::unordered_map<uint32_t, uint32_t> primToInstance;

    void clear() {
        meshInstances.clear();
        lights.clear();
        primToInstance.clear();
    }
};
