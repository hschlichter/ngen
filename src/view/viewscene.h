#pragma once

#include "material.h"
#include "mesh.h"
#include "renderworld.h"
#include "scenepacktypes.h"
#include "world.h"
#include "worldcomposer.h"

#include <glm/glm.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

class AssetClient;
struct PackedTextureView;

// A local matrix as position, rotation and scale, as the gizmos take it (shear is dropped).
auto decomposeTransform(const glm::mat4& local) -> Transform;

// A prim's transforms as the view draws it: local, and world before the unit scale.
struct EntityTransform {
    glm::mat4 local = glm::mat4(1.0f);
    glm::mat4 world = glm::mat4(1.0f);
    bool xformable = false;
    bool resetsXformStack = false;
};

// A camera the scene authors (a UsdGeom Camera prim): its full path and its world transform in metres. It looks down its local -Z with
// local +Y up, as USD's cameras do.
struct SceneCamera {
    std::string path;
    glm::mat4 worldTransform = glm::mat4(1.0f);
};

// The scene as the scene thread last published it, for the main thread's queries. Immutable once published.
struct SceneSnapshot {
    std::string id;
    uint64_t revision = 0;
    RenderWorld drawn;
    std::vector<std::string> primPaths;                                  // by prim index - 1
    std::unordered_map<std::string, std::vector<uint32_t>> drawsByAsset; // asset id to the prims whose draws use it
    std::unordered_map<uint32_t, std::vector<std::string>> assetsByPrim; // a drawn prim to the assets its draws use
    std::map<std::string, EntityTransform> transforms;                   // every prim reached while drawing, by full path
    std::vector<SceneCamera> cameras;                                    // in visit order
    glm::mat4 unitScale = glm::mat4(1.0f);
    std::vector<std::string> assetIds; // every asset the scene uses: layer packs, meshes, materials, textures, failed ones
    // The World, summarised.
    std::vector<std::string> layerStack;
    std::string upAxis = "Y";
    double metersPerUnit = 0.01;
    size_t prims = 0;
    size_t prototypes = 0;
    std::unordered_map<std::string, uint32_t> lodLevels;
    // Nothing queued, requested or waiting to be extracted when it was published.
    bool idle = true;
};

// The scene ngen-view shows: its World, composed from the scene's packs, and the renderer's inputs built from it: the draws (RenderWorld,
// one slot per prim submesh) and the meshes, materials and textures they use, each kept by asset across rebuilds. Everything comes from
// the asset server; nothing of USD. See src/view/README.md.
//
// Nothing here waits. A scene thread owns the composer, the packs and the libraries: it takes requests (open, LOD, purposes, refresh,
// transforms) and the assets that arrive, composes and extracts per batch, and publishes a SceneSnapshot and what changed for the
// renderer (takeChanges). The main thread asks through the requests and reads the latest snapshot, taken once a frame by update().
class ViewScene {
public:
    explicit ViewScene(AssetClient& assets);
    ~ViewScene();
    ViewScene(const ViewScene&) = delete;
    auto operator=(const ViewScene&) -> ViewScene& = delete;

    // Requests; each returns at once and is applied by the scene thread in order.
    // Opens the scene with this asset id (its root layer); an empty id is an empty scene.
    auto open(const std::string& sceneId) -> void;
    // The level a LOD root wants, by its full path; loads it.
    auto setLodLevel(const std::string& rootPath, uint32_t level) -> void;
    // Whether prims of a purpose are drawn: default and render always are, guide and proxy when turned on.
    auto setPurposeShown(Purpose purpose, bool shown) -> void;
    auto purposeShown(Purpose purpose) const -> bool;
    // Fetches these assets again and rebuilds the scene from them; the meshes and materials of a USD file fetched again are fetched again
    // too. `force` has the asset server repack them even when its cache is up to date (the Assets window's Repack); without it, what
    // changed at its source is packed again (a change notice), and ids that aren't the scene's are left alone. With it, an asset that isn't
    // the scene's (a shader) is repacked only on the server.
    auto refresh(const std::vector<std::string>& ids, bool force = true) -> void;
    // Replaces a prim's local transform, as a gizmo drag does: a local opinion of the view, kept until it closes, never saved. Moves
    // what is drawn under the prim; false for a prim that isn't there or takes no transform (as the current snapshot has it).
    auto setLocalTransform(const std::string& path, const glm::mat4& local) -> bool;

    // Once a frame, on the main thread: takes the latest snapshot. True when it is a new one.
    auto update() -> bool;
    // True when the scene thread has nothing queued, requested or unextracted, as of the snapshot update() took and the requests since.
    auto idle() const -> bool;
    // What changed in the renderer's inputs since the last call: the first call after open has everything. Thread-safe.
    auto takeChanges() -> SceneChanges;

    // Queries over the snapshot update() took.
    auto sceneId() const -> const std::string& { return current->id; }
    auto snapshot() const -> const SceneSnapshot& { return *current; }
    auto renderWorld() const -> const RenderWorld& { return current->drawn; }
    // The cameras the scene authors, in visit order.
    auto cameras() const -> const std::vector<SceneCamera>& { return current->cameras; }
    // Moves on each time the renderer's inputs change.
    auto revision() const -> uint64_t { return current->revision; }
    // Every asset the scene uses: its layer packs, meshes, materials and textures.
    auto assetIds() const -> std::vector<std::string> { return current->assetIds; }
    // The scene's up axis, as a vector.
    auto worldUp() const -> glm::vec3;
    // The full path of the entity a draw instance or light names (RenderWorld's prim index); empty for none.
    auto primPath(uint32_t prim) const -> std::string;
    // The full path of the entity a draw instance (a slot of RenderWorld::meshInstances) draws; empty for none.
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

private:
    struct OpenRequest {
        std::string id;
    };
    struct LodRequest {
        std::string root;
        uint32_t level = 0;
    };
    struct PurposeRequest {
        Purpose purpose = Purpose::Default;
        bool shown = false;
    };
    struct RefreshRequest {
        std::vector<std::string> ids;
        bool force = false;
    };
    struct TransformRequest {
        std::string path;
        glm::mat4 local = glm::mat4(1.0f);
    };
    using Request = std::variant<OpenRequest, LodRequest, PurposeRequest, RefreshRequest, TransformRequest>;
    // What an asset the scene thread asked for is, so its arrival goes to the right place.
    enum class AssetKind : uint8_t {
        Layer,
        Mesh,
        Material,
        Texture,
        Other, // repacked for someone else (a shader): only answered
    };

    auto post(Request request) -> void;
    auto run(const std::stop_token& stop) -> void;

    // The scene thread's.
    auto apply(Request& request) -> void;
    auto applyOpen(const std::string& sceneId) -> void;
    auto applyRefresh(const std::vector<std::string>& ids, bool force) -> void;
    auto applyTransform(const std::string& path, const glm::mat4& local) -> void;
    auto receive(const std::string& assetId) -> void;
    auto requestAssets(const std::vector<std::pair<std::string, AssetKind>>& wanted, bool force) -> void;
    // Composes with the layer packs held and requests the ones it names and lacks.
    auto compose() -> void;
    // Requests the meshes and materials of what is drawn and the textures of the materials held.
    auto requestDrawnAssets() -> void;
    auto extract() -> void;
    auto publish() -> void;
    auto drawsPurpose(Purpose purpose) const -> bool;
    auto material(const std::string& materialAsset, const std::optional<std::array<float, 3>>& displayColor) -> MaterialHandle;
    // The library entry for a texture asset, or for a constant colour's generated 1x1; added on first use.
    auto texture(const std::string& textureAsset) -> TextureHandle;
    auto constantTexture(const glm::vec4& color) -> TextureHandle;
    // The library entry for a mesh asset, added on first use; null when its pack hasn't arrived.
    auto mesh(const std::string& meshAsset) -> MeshHandle;
    // Records what extract() used, against what the last one did, as changes for the renderer.
    auto collectChanges(const std::set<uint32_t>& meshesNow, const std::set<uint32_t>& materialsNow, const std::set<uint32_t>& texturesNow, SceneChanges& changes) -> void;
    auto addChanges(SceneChanges changes) -> void;

    AssetClient& assets;

    // Between the threads.
    mutable std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<Request> requests;
    bool answered = false;                          // the asset client answered something since the scene thread last looked
    std::shared_ptr<const SceneSnapshot> published; // the latest, for update()
    SceneChanges pending;                           // what changed since takeChanges() last ran
    std::atomic<uint64_t> requestsPosted{0};
    std::atomic<uint64_t> requestsApplied{0};

    // The main thread's.
    std::shared_ptr<const SceneSnapshot> current = std::make_shared<SceneSnapshot>();
    std::set<Purpose> shownPurposes = {Purpose::Default, Purpose::Render};

    // The scene thread's, from here on.
    std::string id;
    std::unique_ptr<WorldComposer> composer;
    std::set<Purpose> drawnPurposes = {Purpose::Default, Purpose::Render};
    std::set<std::string> requestedPacks;
    std::unordered_map<std::string, AssetKind> inFlight; // requested and not answered yet
    std::set<std::string> refreshing;                    // asked to be fetched again: their arrival replaces what the scene holds
    bool composeNeeded = false;
    bool extractNeeded = false;
    bool composeReported = false; // PacksComposed traced since the scene opened or a pack last arrived
    bool assetsReported = false;  // SceneAssetsReceived traced since the last asset was requested
    size_t assetsSinceReport = 0;
    std::chrono::steady_clock::time_point openedAt;

    // Packed assets as they arrived, by asset id; a failed one is absent and listed in `failedAssets`. Versions, to keep a library entry
    // whose asset arrives again unchanged.
    std::unordered_map<std::string, MeshPack> meshPacks;
    std::unordered_map<std::string, MaterialPack> materialPacks;
    std::unordered_map<std::string, std::shared_ptr<const TextureDesc>> texturePacks;
    std::unordered_map<std::string, std::string> packVersions;
    std::set<std::string> failedAssets;

    // The renderer's inputs. The libraries keep their entries across extracts; extract() assigns each draw a slot by its key (prim path and
    // submesh), reusing a removed draw's slot, lowest first.
    RenderWorld drawn;
    MeshLibrary meshLibrary;
    MaterialLibrary materialLibrary;
    TextureLibrary textureLibrary;
    std::unordered_map<uint32_t, AABB> meshBoundsOf; // by mesh handle
    std::set<uint32_t> usedMeshes;                   // what the last extract drew with, by handle
    std::set<uint32_t> usedMaterials;
    std::set<uint32_t> usedTextures;
    std::unordered_map<std::string, MaterialHandle> materialByAsset; // this extract's, by material asset id or "displayColor r g b"
    std::unordered_map<std::string, uint32_t> slotOfDraw;            // "<prim path>#<submesh>" to its slot
    std::set<uint32_t> freeSlots;
    std::vector<std::string> primPaths;
    std::unordered_map<std::string, std::vector<uint32_t>> drawsByAsset;
    std::unordered_map<uint32_t, std::vector<std::string>> assetsByPrim;
    std::map<std::string, EntityTransform> transforms;
    std::vector<SceneCamera> authoredCameras;
    std::map<std::string, glm::mat4> localOverrides; // setLocalTransform's, applied by every extract
    glm::mat4 unitScale = glm::mat4(1.0f);
    uint64_t inputsRevision = 0;

    std::jthread thread; // last: started when everything above exists
};
