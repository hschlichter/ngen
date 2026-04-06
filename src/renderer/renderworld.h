#pragma once

#include "scenehandles.h"
#include "scenetypes.h"

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
    glm::mat4 worldTransform = glm::mat4(1.0f);
};

struct RenderWorld {
    std::vector<RenderMeshInstance> meshInstances;
    std::vector<RenderLight> lights;

    void clear() {
        meshInstances.clear();
        lights.clear();
    }
};
