#pragma once

#include "material.h"
#include "mesh.h"
#include "scenehandles.h"
#include "scenetypes.h"

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

// One draw: a submesh of a placed mesh. Draws live in slots that keep their index while they exist; a removed draw's slot is dead (alive
// false) until a new draw reuses it.
struct RenderMeshInstance {
    MeshHandle mesh;
    MaterialHandle material;
    uint32_t prim = 0; // the prim index (ViewScene::primPath) of the prim it draws
    glm::mat4 worldTransform = glm::mat4(1.0f);
    AABB worldBounds;
    // A prim's mesh expands to one instance per material submesh, all sharing
    // the same mesh buffers and transform. [indexOffset, indexOffset+indexCount)
    // is this submesh's range in the mesh index buffer; primFirst marks the
    // first instance of the prim so the shadow pass can draw the whole mesh once.
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    bool primFirst = true;
    bool doubleSided = false;
    bool cutout = false;        // its material is cutout: drawn by the cutout pipelines
    bool primHasCutout = false; // some submesh of its prim is cutout: the shadow pass draws the prim per submesh
    uint32_t lodLevel = 0;      // the drawn level + 1 of the nearest LOD root above its prim, 0 under none; for the LOD level view
    bool alive = true;

    auto operator==(const RenderMeshInstance&) const -> bool = default;
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

    auto operator==(const RenderLight&) const -> bool = default;
};

struct RenderWorld {
    std::vector<RenderMeshInstance> meshInstances; // by slot
    std::vector<RenderLight> lights;
    // Each drawn prim's slots, one per submesh.
    std::unordered_map<uint32_t, std::vector<uint32_t>> primInstances;
};

// What changed in the renderer's inputs, in order. The view builds it by comparing what it draws with what it drew; the renderer applies
// it to the GPU scene. Descriptions are shared: the renderer keeps them while their uploads are pending.
struct SceneChanges {
    std::vector<std::pair<MeshHandle, std::shared_ptr<const MeshDesc>>> meshesAdded;
    std::vector<MeshHandle> meshesRemoved;
    std::vector<std::pair<TextureHandle, std::shared_ptr<const TextureDesc>>> texturesAdded;
    std::vector<TextureHandle> texturesRemoved;
    std::vector<std::pair<MaterialHandle, MaterialDesc>> materialsChanged; // added, or with new content
    std::vector<MaterialHandle> materialsRemoved;
    // Draws whose slot changed: added, updated or removed (alive false). slotCount is the slot list's length afterwards.
    std::vector<std::pair<uint32_t, RenderMeshInstance>> instances;
    uint32_t slotCount = 0;
    bool lightsChanged = false;
    std::vector<RenderLight> lights;

    auto empty() const -> bool {
        return meshesAdded.empty() && meshesRemoved.empty() && texturesAdded.empty() && texturesRemoved.empty() && materialsChanged.empty() && materialsRemoved.empty() &&
               instances.empty() && !lightsChanged;
    }
    // Appends `later`, which applies after this.
    auto append(SceneChanges later) -> void {
        for (auto& entry : later.meshesAdded) {
            meshesAdded.push_back(std::move(entry));
        }
        meshesRemoved.insert(meshesRemoved.end(), later.meshesRemoved.begin(), later.meshesRemoved.end());
        for (auto& entry : later.texturesAdded) {
            texturesAdded.push_back(std::move(entry));
        }
        texturesRemoved.insert(texturesRemoved.end(), later.texturesRemoved.begin(), later.texturesRemoved.end());
        for (auto& entry : later.materialsChanged) {
            materialsChanged.push_back(std::move(entry));
        }
        materialsRemoved.insert(materialsRemoved.end(), later.materialsRemoved.begin(), later.materialsRemoved.end());
        for (auto& entry : later.instances) {
            instances.push_back(std::move(entry));
        }
        slotCount = later.slotCount;
        if (later.lightsChanged) {
            lightsChanged = true;
            lights = std::move(later.lights);
        }
    }
};
