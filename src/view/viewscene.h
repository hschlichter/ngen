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
#include <functional>
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
    bool absolute = false; // kept relative to a content: a !resetXformStack! in it, so the content's placement doesn't apply
};

// The prims' transforms, without a map of every prim: each content's prims relative to the content's root, kept with the content, and
// every place a content is used (the stage, each instance, each drawn level) with its world transform. Immutable once published.
using ContentTransforms = std::unordered_map<std::string, EntityTransform>; // by path relative to the content's root
struct TransformIndex {
    struct Use {
        std::shared_ptr<const ContentTransforms> prims;
        glm::mat4 world = glm::mat4(1.0f);
    };
    std::unordered_map<std::string, Use> uses;                  // by the use's full path
    std::unordered_map<std::string, EntityTransform> overrides; // moved by a gizmo since the last extract, world absolute
    // A prim's transform, world before the unit scale; none when it isn't reached (inactive, invisible, in a level not drawn).
    auto find(const std::string& path) const -> std::optional<EntityTransform>;
};

// A camera the scene authors (a UsdGeom Camera prim): its full path and its world transform in metres. It looks down its local -Z with
// local +Y up, as USD's cameras do.
struct SceneCamera {
    std::string path;
    glm::mat4 worldTransform = glm::mat4(1.0f);
};

// A LOD root as drawn: its full path (instances expanded), its levels' names finest first, the level it defaults to, wants and draws, and the
// levels loaded. `drawn` is empty while no level is loaded.
struct SceneLodRoot {
    std::string path;
    std::vector<std::string> levels;
    std::vector<std::vector<std::string>> levelPayloads; // per level, the asset ids of its payloads; empty for a level without
    std::string heuristicType;                           // empty when the root names none
    bool heuristicChosen = false;                        // a heuristic the engine reads, with valid bounds; else the root keeps its default
    std::vector<float> thresholds;
    std::array<glm::vec3, 2> extent = {glm::vec3(-1.0f), glm::vec3(1.0f)}; // the heuristic's, in the root's local space
    bool projectedExtent = false;                                          // projectionMethod: the box, else its bounding sphere
    uint32_t defaultIndex = 0;
    uint32_t wanted = 0;
    std::optional<uint32_t> drawn;
    std::vector<uint32_t> loaded;
    bool setByHand = false;
    glm::mat4 worldTransform = glm::mat4(1.0f); // the root's, in metres: places the heuristic's extent
    // While it wants a loaded level it doesn't draw yet: what that level waits for, counted through its payloads and instances (each
    // content and mesh once): contents not composed with every pack they named, and meshes not held, of them those asked for.
    struct Waiting {
        uint32_t contentsIncomplete = 0;
        uint32_t meshesMissing = 0;
        uint32_t meshesInFlight = 0;
    };
    std::optional<Waiting> waiting;
};

// Whether a LOD root's heuristic extent (its bounding sphere, placed by the root's transform) is inside a view's frustum, as UsdLod's
// reference query sees it: a root outside measures 0 there. The view's heuristic ignores the frustum; the `streaming` record reports it.
auto lodRootInFrustum(const SceneLodRoot& root, const glm::vec3& eye, const glm::vec3& forward, const glm::vec3& right, float fovY, float aspect) -> bool;

// What the LOD heuristic chose for a root at the last evaluation: its screen size (the fraction of the view its extent's bounding sphere
// covers, -1 when not computed) and the level it wants, with why: "heuristic", "hand-set", "heuristic off", "no screen-size heuristic",
// "no camera".
struct SceneLodChoice {
    float screenSize = -1.0f;
    uint32_t level = 0;
    std::string reason;
};
using SceneLodChoices = std::unordered_map<std::string, SceneLodChoice>; // by the root's full path

// The scene thread's work as of a snapshot: batches of requests and answers taken, composes (each composes with the packs held and asks for
// those it names and lacks) and extracts (the renderer's inputs built from the World), the assets in flight by kind, and what it holds.
struct SceneThreadStats {
    uint64_t batches = 0;
    uint64_t composes = 0;
    double lastComposeMs = 0.0; // the last compose that had every layer pack it named
    // The last compose: contents composed and those kept from before (prototypes and LOD level contents), and how many it reached.
    uint32_t prototypesComposed = 0;
    uint32_t levelsComposed = 0;
    uint32_t prototypesHeld = 0;
    uint32_t levelsHeld = 0;
    uint32_t contentsReached = 0;
    double composeStageMs = 0.0;
    double composeResolveMs = 0.0;
    double composeFinishMs = 0.0;
    double lastComposeCallMs = 0.0; // every compose, whether or not it had every pack
    uint64_t extracts = 0;
    double lastExtractMs = 0.0;
    double extractVisitMs = 0.0; // of the last extract: walking the World
    double extractSlotsMs = 0.0; // the library changes and the slots
    uint64_t extractReused = 0;  // of the last extract: content walks taken from the kept ones
    uint64_t extractWalked = 0;  // and walked (new, composed again, or with a gizmo's opinion in them)
    uint64_t extractKept = 0;    // kept walks after it
    uint64_t extractsHeld = 0;   // batches whose extract waited while assets arrived
    // The LOD heuristic's last evaluation: roots evaluated, levels it changed (finer, coarser), and its time.
    uint32_t lodRootsEvaluated = 0;
    uint32_t lodFiner = 0;
    uint32_t lodCoarser = 0;
    // Unloading, since the scene opened: mesh packs released, and composed contents the composer let go.
    uint64_t releasedMeshes = 0;
    uint64_t releasedMeshBytes = 0;
    uint64_t contentsEvicted = 0;
    double lodEvaluateMs = 0.0;
    uint64_t lodEvaluations = 0;
    uint64_t relinks = 0;
    double publishMs = 0.0; // the last snapshot's
    uint64_t assetsReceived = 0;
    uint32_t inFlightLayers = 0;
    uint32_t inFlightMeshes = 0;
    uint32_t inFlightMaterials = 0;
    uint32_t inFlightTextures = 0;
    uint32_t inFlightOther = 0;
    uint32_t layerPacks = 0; // requested since the scene opened
    uint32_t meshPacks = 0;
    uint32_t materialPacks = 0;
    uint32_t texturePacks = 0;
    uint32_t failedAssets = 0;
    uint32_t meshEntries = 0; // library entries the renderer has been told about
    uint32_t materialEntries = 0;
    uint32_t textureEntries = 0;
    bool composeNeeded = false;
    bool extractNeeded = false;
    double sinceOpenMs = 0.0;
};

// A payload placement reached while drawing: the prim with payloads, the asset ids, whether they are loaded, and the bounds authored for it
// (extentsHint) in world metres; none when it authors none.
struct ScenePayload {
    std::string path;
    std::vector<std::string> assetIds;
    bool loaded = false;
    std::optional<AABB> worldBounds;
};

// What an extract built for the renderer and the main thread's queries, shared by every snapshot published until the next extract (or a
// gizmo's move, which publishes a copy with the moved draws). Immutable once published.
struct SceneDraws {
    RenderWorld drawn;
    std::vector<std::string> primPaths;                                        // by prim index - 1
    std::unordered_map<std::string, std::vector<uint32_t>> drawsByAsset;       // asset id to the prims whose draws use it
    std::vector<std::shared_ptr<const std::vector<std::string>>> assetsByPrim; // by prim index - 1: the assets a drawn prim's draws use
    std::vector<SceneCamera> cameras;                                          // in visit order
    std::vector<SceneLodRoot> lodRoots;                                        // every LOD root reached while drawing, in visit order
    std::vector<ScenePayload> payloads;                                        // every payload placement reached while drawing, in visit order
};

// The scene as the scene thread last published it, for the main thread's queries. Immutable once published.
struct SceneSnapshot {
    std::string id;
    uint64_t revision = 0;
    std::shared_ptr<const SceneDraws> draws = std::make_shared<SceneDraws>();
    std::shared_ptr<const TransformIndex> transforms = std::make_shared<TransformIndex>(); // every prim reached while drawing
    std::shared_ptr<const SceneLodChoices> lodChoices = std::make_shared<SceneLodChoices>();
    bool lodHeuristic = true;
    double unloadDelay = 10.0;
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
    SceneThreadStats thread;
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
    // Puts a LOD root back to its default level (lod:default:index), or to the heuristic's choice while the heuristic is on.
    auto resetLodLevel(const std::string& rootPath) -> void;
    // The view's camera, for the LOD heuristic: position in metres, vertical field of view in radians, aspect (width over height). Only
    // the latest posted counts; the scene thread evaluates the heuristic when it moved.
    auto setCamera(const glm::vec3& position, float fovY, float aspect) -> void;
    // The aspect of the camera last posted (1 before one is).
    auto cameraAspect() const -> float;
    // The LOD heuristic on (every root not set by hand wants the level its screen size picks) or off (its default).
    auto setLodHeuristic(bool on) -> void;
    // How long what stopped being drawn or reached is kept before it is unloaded (packs, library entries, GPU copies, composed contents).
    auto setUnloadDelay(double seconds) -> void;
    auto unloadDelay() const -> double { return current->unloadDelay; }
    auto lodHeuristic() const -> bool { return current->lodHeuristic; }
    // What the heuristic chose at its last evaluation, by root path.
    auto lodChoices() const -> const SceneLodChoices& { return *current->lodChoices; }
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
    auto renderWorld() const -> const RenderWorld& { return current->draws->drawn; }
    // The cameras the scene authors, in visit order.
    auto cameras() const -> const std::vector<SceneCamera>& { return current->draws->cameras; }
    // The scene thread's work as of the snapshot update() took, and the requests posted and applied now.
    auto threadStats() const -> const SceneThreadStats& { return current->thread; }
    auto requestCounts() const -> std::pair<uint64_t, uint64_t> { return {requestsPosted.load(), requestsApplied.load()}; }
    // The payload placements reached while drawing, in visit order.
    auto payloads() const -> const std::vector<ScenePayload>& { return current->draws->payloads; }
    // The LOD roots reached while drawing, in visit order.
    auto lodRoots() const -> const std::vector<SceneLodRoot>& { return current->draws->lodRoots; }
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
        std::optional<uint32_t> level; // none: back to the root's default
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
    struct HeuristicRequest {
        std::optional<bool> on;
        std::optional<double> unloadDelay;
    };
    using Request = std::variant<OpenRequest, LodRequest, PurposeRequest, RefreshRequest, TransformRequest, HeuristicRequest>;
    struct CameraInput {
        glm::vec3 position = glm::vec3(0.0f);
        float fovY = 0.785f;
        float aspect = 1.0f;
    };
    // The LOD heuristic over the roots the last extract reached: each one's level from its screen size, with hysteresis, unless set by
    // hand; the composer gets what changed. True when something did.
    auto evaluateLevels() -> bool;
    auto relink() -> void;
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
    auto releaseMeshPack(const MeshPack& pack) -> void; // counts it in the stats; the caller erases it
    auto reportAssets() -> void;
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
    bool working = false;                           // the scene thread took a batch and hasn't published its result yet
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
    bool requestNeeded = false; // what is drawn may name assets not asked for yet: a compose, a material's arrival, a purpose shown
    // The LOD heuristic: the camera the main thread posted (between the threads) and the scene thread's, the levels set by hand, what the
    // heuristic chose last per root (its hysteresis), what the composer was given, and whether to evaluate again.
    std::optional<CameraInput> postedCamera;
    std::optional<CameraInput> camera;
    bool heuristicOn = true;
    bool evaluateNeeded = false;
    std::unordered_map<std::string, uint32_t> handLevels;
    std::unordered_map<std::string, uint32_t> heuristicLevels;
    std::unordered_map<std::string, uint32_t> composerLevels;
    // Unloading: what stopped being drawn (library entries, on the GPU) or reached (composed contents) goes after this long; when the
    // next goes, for the scene thread to wake.
    double unloadDelaySeconds = 10.0;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> meshUnusedSince;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> materialUnusedSince;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> textureUnusedSince;
    std::optional<std::chrono::steady_clock::time_point> releaseDue;
    std::chrono::steady_clock::time_point lastEvict;
    uint64_t reportedReleasedMeshes = 0;
    uint64_t reportedReleasedBytes = 0;
    auto unload() -> void;
    std::shared_ptr<const SceneLodChoices> choices = std::make_shared<SceneLodChoices>();
    std::chrono::steady_clock::time_point lastExtractEnd;
    std::chrono::steady_clock::time_point extractDue; // a held-back extract's time
    bool composeReported = false;                     // PacksComposed traced since the scene opened or a pack last arrived
    bool assetsReported = false;                      // SceneAssetsReceived traced since the last asset was requested
    size_t assetsSinceReport = 0;
    std::chrono::steady_clock::time_point openedAt;

    // Packed assets as they arrived, by asset id; a failed one is absent and listed in `failedAssets`. Versions, to keep a library entry
    // whose asset arrives again unchanged.
    std::unordered_map<std::string, MeshPack> meshPacks;
    // The meshes of every loaded level, as the last request listed them: their packs are held while drawn or not (a root's floor while a
    // finer level is drawn), so a switch back needs no fetch.
    std::set<std::string> meshesLoaded;
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
    std::vector<std::shared_ptr<const std::vector<std::string>>> assetsByPrim;
    std::shared_ptr<const SceneDraws> draws = std::make_shared<SceneDraws>(); // the last extract's, as published
    // Mesh assets and bound materials by a small id, given when a content's walk is kept, so a use looks them up by index.
    std::unordered_map<std::string, uint32_t> meshIds;
    std::unordered_map<std::string, uint32_t> materialIds;
    // Counted when the World is composed, not at every publish: a content's prims (as World::visit reaches them) kept by its version.
    size_t worldPrims = 0;
    std::unordered_map<const WorldContent*, std::pair<uint64_t, size_t>> primCounts;
    auto countPrims(const World& world, const WorldContent& content, bool rootIsPrim) -> size_t;
    auto countWorld() -> void;
    // Every asset id the scene uses, rebuilt when the sets of packs change size.
    std::vector<std::string> assetIdList;
    size_t assetIdKey = SIZE_MAX;
    std::shared_ptr<TransformIndex> transforms = std::make_shared<TransformIndex>();
    // What extract takes from a content, kept per content version and the state the content inherits where it is used (purpose and
    // material binding): its prims in walk order, with transforms relative to the content's root, and where instances and levels in it
    // sit. A use replays it instead of walking the content's entities.
    struct ExtractItem {
        enum class Kind : uint8_t {
            Prim,     // a prim that draws, lights, frames (a camera), places payloads or roots LOD
            Instance, // an instance: its prototype is used here
            Level,    // a linked content: a LOD level (used when it is the level drawn) or a prim with payloads (always)
        };
        Kind kind = Kind::Prim;
        std::string path;                  // relative to the content's root ("/" for a level content's root, the level prim)
        glm::mat4 world = glm::mat4(1.0f); // relative to the content's root
        bool absolute = false;
        const Entity* entity = nullptr; // a Prim's, in the content
        EntityId id = noEntity;
        Purpose purpose = Purpose::Default;
        std::string material; // the binding in force here
        bool strongBinding = false;
        int32_t content = -1;         // an Instance's prototype (a Level's content is its stand-in's link, read at use)
        uint32_t meshId = UINT32_MAX; // a Prim's mesh asset (meshIds), when it has one
        uint32_t materialId = 0;      // its bound material (materialIds)
        bool camera = false;
        bool payload = false;
        bool lodRoot = false;
        bool light = false;
        bool subsets = false;     // its mesh's GeomSubsets may bind their own materials: resolved per prim
        EntityId root = noEntity; // a Level's LOD root, in the content
        uint32_t level = 0;
    };
    struct ContentExtract {
        uint64_t version = 0;
        std::vector<ExtractItem> items;
        std::shared_ptr<const ContentTransforms> transforms;
    };
    std::unordered_map<std::string, std::shared_ptr<const ContentExtract>> extractCache; // by content, version and inherited state
    // A content's walk afresh; its walk kept by version and inherited state (afresh when a gizmo's opinion is under its use).
    auto buildExtract(const WorldContent& content, bool rootIsPrim, Purpose purpose, const std::string& material, bool strong, const std::string& overrideBase) -> std::shared_ptr<const ContentExtract>;
    auto extractOf(char kind, int32_t index, const WorldContent& content, bool rootIsPrim, Purpose purpose, const std::string& material, bool strong, const std::string& usePath) -> std::shared_ptr<const ContentExtract>;
    auto overridesUnder(const std::string& usePath) const -> bool;
    using PrimVisitor = std::function<void(const WorldContent& content, const ExtractItem& item, const std::string& path, const glm::mat4& world, uint32_t lodTag)>;
    // allLoaded: every loaded level of a root is replayed, not only the one it draws (to ask for what a level still loading needs).
    auto replay(const World& world, const PrimVisitor& onPrim, TransformIndex* transformsOut, bool allLoaded = false) -> void;
    // The level a root draws (the switch rule): the finest loaded level not finer than the one it wants that is complete; while none is,
    // its coarsest loaded. Completeness is memoised per extract (readiness).
    auto drawnLevelOf(const World& world, const std::string& rootPath, const LodRoot& root, const WorldContent& content, Purpose purpose) -> std::optional<uint32_t>;
    // A content complete enough to draw: composed with every pack it named, every mesh it draws held (or failed), and the same for what is
    // linked or instanced in it; a LOD root in it by its coarsest level. `purpose` is the one it inherits where it is used: a mesh of a
    // purpose not drawn there isn't asked for, so it isn't waited for.
    auto contentReady(const World& world, char kind, int32_t index, Purpose purpose) -> bool;
    std::unordered_map<int64_t, bool> readiness; // by content (kind and index), this extract's
    // Counts what a content waits for into `waiting`, as contentReady judges it, each content and mesh once (`seen`).
    auto countWaiting(const World& world, char kind, int32_t index, Purpose purpose, std::set<int64_t>& seenContents, std::set<std::string>& seenMeshes, SceneLodRoot::Waiting& waiting) -> void;
    std::unordered_map<std::string, uint32_t> previousDrawnLevels; // by root path, as the last extract drew them: LevelsSwitched
    uint64_t extractCacheHits = 0;
    uint64_t extractCacheBuilds = 0;
    std::vector<SceneCamera> authoredCameras;
    std::vector<SceneLodRoot> drawnLodRoots;
    std::vector<ScenePayload> reachedPayloads;
    SceneThreadStats stats;                          // the counters; publish() fills in the rest
    std::map<std::string, glm::mat4> localOverrides; // setLocalTransform's, applied by every extract
    glm::mat4 unitScale = glm::mat4(1.0f);
    uint64_t inputsRevision = 0;

    std::jthread thread; // last: started when everything above exists
};
