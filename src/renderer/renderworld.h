#pragma once

#include "scenehandles.h"
#include "scenetypes.h"
#include "types.h"

#include <vector>

struct RenderMeshInstance {
    MeshHandle mesh;
    MaterialHandle material;
    glm::mat4 worldTransform = glm::mat4(1.0f);
    AABB worldBounds;

    // Temporary: raw mesh data pointers for GPU upload until proper asset libraries exist
    const std::vector<Vertex>* vertices = nullptr;
    const std::vector<uint32_t>* indices = nullptr;
    const uint8_t* texPixels = nullptr;
    int texWidth = 0;
    int texHeight = 0;
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
