#pragma once

#include "material.h"
#include "mesh.h"
#include "scenehandles.h"
#include "scenetypes.h"

#include <array>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// A list kept in shared chunks, for a producer that publishes copies of a large list it keeps changing: a copy shares every chunk, and a
// write copies only the chunk it changes, and only while a copy still shares it. One thread writes; copies are read anywhere.
template <typename T>
class ChunkedList {
public:
    static constexpr size_t chunkSize = 4096;

    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        Iterator() = default;
        Iterator(const ChunkedList* list, size_t index) : list(list), index(index) {}
        auto operator*() const -> const T& { return (*list)[index]; }
        auto operator->() const -> const T* { return &(*list)[index]; }
        auto operator++() -> Iterator& {
            index++;
            return *this;
        }
        auto operator++(int) -> Iterator {
            auto before = *this;
            index++;
            return before;
        }
        auto operator==(const Iterator& other) const -> bool { return index == other.index; }

    private:
        const ChunkedList* list = nullptr;
        size_t index = 0;
    };

    auto size() const -> size_t { return count; }
    auto empty() const -> bool { return count == 0; }
    auto begin() const -> Iterator { return Iterator(this, 0); }
    auto end() const -> Iterator { return Iterator(this, count); }
    auto operator[](size_t index) const -> const T& { return (*chunks[index / chunkSize])[index % chunkSize]; }
    // The element to change: its chunk is copied first while another list shares it.
    auto edit(size_t index) -> T& { return (*own(index / chunkSize))[index % chunkSize]; }
    auto push_back(T value) -> void {
        if (count % chunkSize == 0) {
            chunks.push_back(std::make_shared<std::vector<T>>());
            chunks.back()->reserve(chunkSize);
        }
        own(chunks.size() - 1)->push_back(std::move(value));
        count++;
    }
    auto clear() -> void {
        chunks.clear();
        count = 0;
    }
    // Each chunk as a contiguous span, in order.
    auto chunkCount() const -> size_t { return chunks.size(); }
    auto chunk(size_t index) const -> std::span<const T> { return *chunks[index]; }
    // A chunk itself, for a reader that keeps what it derived from it: while the reader holds it, a write to the list copies it first, so
    // the same pointer means the same contents.
    auto sharedChunk(size_t index) const -> std::shared_ptr<const std::vector<T>> { return chunks[index]; }

private:
    auto own(size_t chunkIndex) -> std::vector<T>* {
        auto& chunk = chunks[chunkIndex];
        if (chunk.use_count() > 1) {
            auto copy = std::make_shared<std::vector<T>>();
            copy->reserve(chunkSize);
            copy->assign(chunk->begin(), chunk->end());
            chunk = std::move(copy);
        }
        return chunk.get();
    }

    std::vector<std::shared_ptr<std::vector<T>>> chunks;
    size_t count = 0;
};

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
    // is this submesh's range in the mesh index buffer, [meshletOffset, meshletOffset+meshletCount)
    // its meshlets, which draw it in every view; worldBounds is the submesh's box.
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    uint32_t meshletOffset = 0;
    uint32_t meshletCount = 0;
    bool doubleSided = false;
    bool cutout = false;   // its material is cutout: drawn by the cutout pipelines
    uint32_t lodLevel = 0; // the drawn level + 1 of the nearest LOD root above its prim, 0 under none; for the LOD level view
    uint32_t lodDepth = 0; // how many LOD roots its prim is under, 0 under none; the LOD level view's hue
    // Its render-LOD group + 1, 0 for none: drawn only while the group picks its level (lodLevel - 1).
    uint32_t lodGroup = 0;
    // The slot + 1 of the nearest streaming LOD root above its prim, 0 under none: the streaming view colours it by that root's state.
    uint32_t streamRoot = 0;
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

// How a render-LOD group measures the camera: UsdLod's two heuristics.
enum class RenderLodKind : uint32_t {
    ScreenSize, // the sphere over the view window at depth 1, a fraction of the window
    Distance,   // to the center, or the nearest point of the box (0 inside), in metres
};

// A render-LOD root where it is placed: the renderer picks its level each frame from the camera (shaders/lodselect.comp), by the heuristic the
// view chose for it, and draws the slots tagged with it and that level; inside an authored blend band it draws the next level too, the two
// cross-faded. Slots in RenderWorld::lodGroups, like draw slots.
struct RenderLodGroup {
    static constexpr uint32_t maxLevels = 8;
    RenderLodKind kind = RenderLodKind::ScreenSize;
    glm::vec3 center = glm::vec3(0.0f); // screen size: the heuristic extent's bounding sphere, world metres
    float radius = 0.0f;
    glm::mat4 toWorld = glm::mat4(1.0f); // the root's space to world metres, and back (a distance measures in it; kept for both kinds)
    glm::mat4 toRoot = glm::mat4(1.0f);
    glm::vec3 point = glm::vec3(0.0f); // distance: the heuristic's center, in the root's space
    bool hasBox = false;               // distance: measured to this box (the root's space) instead
    glm::vec3 boxMin = glm::vec3(0.0f);
    glm::vec3 boxMax = glm::vec3(0.0f);
    // Finest first, in the kind's unit (a fraction of the window, or metres); levelCount - 1 of them used. A blend threshold per boundary up
    // to blendCount: the band past the threshold where the next level fades in.
    std::array<float, maxLevels - 1> thresholds = {};
    std::array<float, maxLevels - 1> blendThresholds = {};
    uint32_t thresholdCount = 0; // authored, at most levelCount - 1
    uint32_t blendCount = 0;
    uint32_t levelCount = 0;
    uint32_t defaultLevel = 0;
    int32_t forcedLevel = -1; // hand-set, or the default while the heuristic is off; -1 for the heuristic's
    bool alive = false;

    auto operator==(const RenderLodGroup&) const -> bool = default;
};

// The scene's sky, from its dome light: drawn where nothing is, and lighting the scene with its irradiance in place of the hemisphere
// ambient. Without one (or before its texture arrives) the renderer draws the clear colour and the configured ambient.
struct RenderEnvironment {
    TextureHandle texture;                    // an Rgb9e5 lat-long
    glm::vec3 radiance = glm::vec3(1.0f);     // color × intensity × 2^exposure, scaling the image
    glm::mat3 toImage = glm::mat3(1.0f);      // a world direction into the image's frame (its pole +Z, packedtexture.h)
    std::array<glm::vec3, 9> irradiance = {}; // the image's, in its frame (TextureDesc::irradiance)
    PrimHandle primHandle;

    auto operator==(const RenderEnvironment&) const -> bool = default;
};

struct RenderWorld {
    ChunkedList<RenderMeshInstance> meshInstances; // by slot
    ChunkedList<RenderLodGroup> lodGroups;         // by group slot
    std::vector<RenderLight> lights;
    std::optional<RenderEnvironment> environment;
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
    // Render-LOD groups whose slot changed, applied with the draws; lodGroupCount is the group list's length afterwards.
    std::vector<std::pair<uint32_t, RenderLodGroup>> lodGroups;
    uint32_t lodGroupCount = 0;
    bool lightsChanged = false;
    std::vector<RenderLight> lights;
    bool environmentChanged = false;
    std::optional<RenderEnvironment> environment;

    auto empty() const -> bool {
        return meshesAdded.empty() && meshesRemoved.empty() && texturesAdded.empty() && texturesRemoved.empty() && materialsChanged.empty() && materialsRemoved.empty() &&
               instances.empty() && lodGroups.empty() && !lightsChanged && !environmentChanged;
    }
    // Appends `later`, which applies after this.
    auto append(SceneChanges later) -> void {
        // Each list taken whole while this one is empty (the usual case: the consumer takes the changes every frame), else added to.
        auto join = [](auto& into, auto& from) {
            if (into.empty()) {
                into = std::move(from);
                return;
            }
            into.insert(into.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
        };
        join(meshesAdded, later.meshesAdded);
        join(meshesRemoved, later.meshesRemoved);
        join(texturesAdded, later.texturesAdded);
        join(texturesRemoved, later.texturesRemoved);
        join(materialsChanged, later.materialsChanged);
        join(materialsRemoved, later.materialsRemoved);
        join(instances, later.instances);
        slotCount = later.slotCount;
        join(lodGroups, later.lodGroups);
        lodGroupCount = later.lodGroupCount;
        if (later.lightsChanged) {
            lightsChanged = true;
            lights = std::move(later.lights);
        }
        if (later.environmentChanged) {
            environmentChanged = true;
            environment = std::move(later.environment);
        }
    }
};
