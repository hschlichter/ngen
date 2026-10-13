#pragma once

#include "material.h"
#include "mesh.h"
#include "renderworld.h"
#include "scenepacktypes.h"
#include "world.h"
#include "worldcomposer.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class AssetClient;
struct PackedTextureView;

// A local matrix as position, rotation and scale, as the gizmos take it (shear is dropped).
auto decomposeTransform(const glm::mat4& local) -> Transform;

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
    // Fetches these assets again and rebuilds the scene from them; the meshes and materials of a USD file fetched again are fetched again
    // too. Blocks. `force` has the asset server repack them even when its cache is up to date (the Assets window's Repack); without it,
    // what changed at its source is packed again (a change notice), and ids that aren't the scene's are left alone. With it, an asset that
    // isn't the scene's (a shader) is repacked only on the server. Returns how many of the ids are the scene's.
    auto refresh(const std::vector<std::string>& ids, bool force = true) -> size_t;
    // Every asset the scene uses: its layer packs, meshes, materials and textures.
    auto assetIds() const -> std::vector<std::string>;

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
    // The full path of the entity a draw instance (RenderWorld::meshInstances' index) draws; empty for none.
    auto instancePath(uint32_t instance) const -> std::string;
    // The world bounds of what is drawn at or under a path; invalid when nothing is.
    auto bounds(const std::string& path) const -> AABB;
    // The draws (prim indices, for primPath and primBounds) that use an asset: a mesh's prims, a material's or a texture's users, the prims
    // drawing the meshes a USD file defines. Empty for an asset nothing draws (a shader, a file that only overrides or arranges others).
    auto drawsOf(const std::string& assetId) const -> std::vector<uint32_t>;
    auto drawCount(const std::string& assetId) const -> size_t;
    // The world bounds of one drawn prim.
    auto primBounds(uint32_t prim) const -> AABB;
    // The assets used by what is drawn at or under a path: meshes, the USD files defining them, materials and their textures; sorted.
    auto assetsUnder(const std::string& path) const -> std::vector<std::string>;

    // A prim's transforms as the view draws it: local, and world in metres. `xformable` is false for a prim whose type takes no
    // transform (its world is its parent's).
    struct PrimTransform {
        glm::mat4 local = glm::mat4(1.0f);
        glm::mat4 world = glm::mat4(1.0f);
        bool xformable = false;
    };
    // Nullopt when there is no prim at the path (or it is under one that isn't drawn: inactive, invisible, a level not drawn).
    auto transform(const std::string& path) const -> std::optional<PrimTransform>;
    // Replaces a prim's local transform, as a gizmo drag does: a local opinion of the view, kept until it closes, never saved. Moves
    // what is drawn under the prim; false for a prim that isn't there or takes no transform.
    auto setLocalTransform(const std::string& path, const glm::mat4& local) -> bool;

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
    std::unordered_map<std::string, MaterialHandle> materialByAsset;     // by material asset id, or "displayColor r g b" for none
    std::vector<std::string> primPaths;                                  // by prim index - 1
    std::unordered_map<std::string, std::vector<uint32_t>> drawsByAsset; // asset id to the prims whose draws use it
    std::unordered_map<uint32_t, std::vector<std::string>> assetsByPrim; // a drawn prim to the assets its draws use
    // Every prim reached while drawing, by full path: its local transform and its world one before the unit scale.
    struct EntityTransform {
        glm::mat4 local = glm::mat4(1.0f);
        glm::mat4 world = glm::mat4(1.0f);
        bool xformable = false;
        bool resetsXformStack = false;
    };
    std::map<std::string, EntityTransform> transforms;
    std::map<std::string, glm::mat4> localOverrides; // setLocalTransform's, applied by every extract
    glm::mat4 unitScale = glm::mat4(1.0f);
    uint64_t inputsRevision = 0;
};
