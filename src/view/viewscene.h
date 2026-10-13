#pragma once

#include "material.h"
#include "mesh.h"
#include "renderworld.h"
#include "scenepacktypes.h"
#include "world.h"
#include "worldcomposer.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class AssetClient;
struct PackedTextureView;

// The scene ngen-view shows: its World, composed from the scene's packs, and the renderer's inputs built from it (RenderWorld,
// MeshLibrary, MaterialLibrary). Everything comes from the asset server; nothing of USD. See src/view/README.md.
//
// Opening a scene blocks until it is composed and every mesh, material and texture it draws has arrived. So does changing what is
// drawn (a LOD level, a purpose), which loads what the change needs; `revision()` then moves on, and the view uploads the new inputs.
class ViewScene {
public:
    explicit ViewScene(AssetClient& assets);

    // Opens the scene with this asset id (its root layer). False when the root layer can't be had; an empty id is an empty scene.
    auto open(const std::string& sceneId) -> bool;

    // The level a LOD root wants, by its full path; loads it.
    auto setLodLevel(const std::string& rootPath, uint32_t level) -> void;
    // Whether prims of a purpose are drawn: default and render always are, guide and proxy when turned on.
    auto setPurposeShown(Purpose purpose, bool shown) -> void;
    auto purposeShown(Purpose purpose) const -> bool;

    auto sceneId() const -> const std::string& { return id; }
    auto world() const -> const World& { return composer->world(); }
    auto renderWorld() const -> const RenderWorld& { return drawn; }
    auto meshes() const -> std::shared_ptr<const MeshLibrary> { return meshLibrary; }
    auto materials() const -> std::shared_ptr<const MaterialLibrary> { return materialLibrary; }
    // Moves on each time the renderer's inputs change.
    auto revision() const -> uint64_t { return inputsRevision; }
    // The scene's up axis, as a vector.
    auto worldUp() const -> glm::vec3;
    // The full path of the entity a draw instance or light names (RenderWorld's prim index); empty for none.
    auto primPath(uint32_t prim) const -> std::string;
    // The world bounds of what is drawn at or under a path; invalid when nothing is.
    auto bounds(const std::string& path) const -> AABB;

private:
    auto compose() -> void;
    auto fetchAssets() -> void;
    auto extract() -> void;
    auto rebuild() -> void;
    auto drawsPurpose(Purpose purpose) const -> bool;
    auto material(const std::string& materialAsset, const std::optional<std::array<float, 3>>& displayColor) -> MaterialHandle;

    AssetClient& assets;
    std::string id;
    std::unique_ptr<WorldComposer> composer;
    std::set<std::string> requestedPacks;
    std::set<Purpose> shownPurposes = {Purpose::Default, Purpose::Render};

    // Packed assets as they arrived, by asset id; a failed one is absent and listed in `failedAssets`.
    std::unordered_map<std::string, MeshPack> meshPacks;
    std::unordered_map<std::string, MaterialPack> materialPacks;
    std::unordered_map<std::string, std::vector<std::byte>> texturePacks;
    std::set<std::string> failedAssets;

    // The renderer's inputs, rebuilt by extract().
    RenderWorld drawn;
    std::shared_ptr<const MeshLibrary> meshLibrary = std::make_shared<MeshLibrary>();
    std::shared_ptr<const MaterialLibrary> materialLibrary = std::make_shared<MaterialLibrary>();
    std::unordered_map<std::string, MaterialHandle> materialByAsset; // by material asset id, or "displayColor r g b" for none
    std::vector<std::string> primPaths;                              // by prim index - 1
    uint64_t inputsRevision = 0;
};
