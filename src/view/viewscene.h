#pragma once

#include "config.h"

#include "material.h"
#include "mesh.h"
#include "placement.h"
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

// Whether a LOD root's heuristic extent (its bounding sphere, placed by the root's transform) is inside a view's frustum, as UsdLod's
// reference query sees it: a root outside measures 0 there. The view's heuristic ignores the frustum; the `streaming` record reports it.
// A LOD heuristic's measure of a root from an eye in world metres, in the heuristic's unit: a distance in the stage's units (as its
// thresholds), as UsdLodDistanceHeuristicQuery::ComputeDistance; or a screen size, the extent's bounding sphere over the view window at depth
// 1 (`windowArea` its area) as a fraction of it, as UsdLodScreenSizeHeuristicQuery's projectedSphere without its frustum test (what is near
// stays loaded whichever way the camera looks). `rootToWorld` places the root in world metres.
auto lodMeasure(const LodHeuristic& heuristic, const glm::mat4& rootToWorld, double metersPerUnit, const glm::vec3& eye, float windowArea) -> float;
// The view window's area at depth 1 for a vertical field of view and an aspect.
auto lodWindowArea(float fovY, float aspect) -> float;
auto lodRootInFrustum(const SceneLodRoot& root, const glm::vec3& eye, const glm::vec3& forward, const glm::vec3& right, float fovY, float aspect) -> bool;

// What the LOD heuristic chose for a root at the last evaluation: its screen size (the fraction of the view its extent's bounding sphere
// covers, -1 when not computed) and the level it wants, with why: "heuristic", "hand-set", "heuristic off", "no screen-size heuristic",
// "no camera".
// The hysteresis at a LOD boundary without a blend band (pickLodLevel) until the tuning changes it: 20% of a screen size, the same margin
// in a distance. lodselect.comp takes the tuned value as a push constant.
inline constexpr float lodHysteresis = 0.2f;

// One heuristic's thresholds set by the view in place of the authored ones, in the heuristic's units (a distance in the stage's units, a
// screen size as a fraction of the view); a list not set keeps the authored one. A streaming heuristic's thresholds are its load radii and
// `unloadThresholds` its unload radii.
struct LodOverride {
    std::optional<std::vector<float>> thresholds;
    std::optional<std::vector<float>> blendThresholds;
    std::optional<std::vector<float>> unloadThresholds;

    auto operator==(const LodOverride&) const -> bool = default;
};

// The view's LOD tuning: local opinions kept until the view closes, never saved (writing them to USD is the editor's).
// - bias: every root picks its level as if it were `bias` times closer (2: finer everywhere). Applied to the thresholds, the same pick: a
//   distance's times the bias, a screen size's over its square; the measures shown stay what the camera sees.
// - hysteresis: the margin at a boundary without a blend band, a fraction of a screen size.
// - blending: off drops every blend band (no fades, the hysteresis everywhere).
// - overrides: by authored root (lodRootKey: the content the root is authored in and its path there, so every instance of an instanceable
//   asset shares one) and heuristic index.
struct LodTuning {
    float bias = 1.0f;
    float hysteresis = lodHysteresis;
    bool blending = true;
    std::map<std::string, std::map<uint32_t, LodOverride>> overrides;

    auto operator==(const LodTuning&) const -> bool = default;
};

// The override set for one heuristic of an authored root; null for none.
auto lodOverrideOf(const LodTuning& tuning, const std::string& rootKey, uint32_t heuristic) -> const LodOverride*;
// A heuristic as the view picks with it under the tuning: the override's thresholds, the bias applied to them, no bands with blending off.
auto tunedLodHeuristic(const LodHeuristic& heuristic, const LodOverride* override, const LodTuning& tuning) -> LodHeuristic;

// What the view knows of a render-LOD group's root, published with the draws for the inspector: its authored root (lodRootKey), its levels'
// names finest first, and every heuristic it authors.
struct LodGroupSource {
    std::string key;
    uint32_t depth = 1; // how many LOD roots it is in, itself counted: the LOD Level view's hue
    std::vector<std::string> levels;
    std::vector<LodHeuristic> heuristics;
};

// What the heuristic chose for a root at its last evaluation: the heuristic it measured by (an index into the root's heuristics, -1 for
// none), its kind and its measure (a distance in the stage's units, or a screen size; -1 when not measured), the level, and why.
// Residency: the streaming heuristic it measured by (-1 for none) and its measure, the state (the finest level that must be resident, its
// level and every coarser one resident), the levels asked of the composer and why, and what of the authoring contract doesn't hold.
struct SceneLodChoice {
    int32_t heuristic = -1;
    LodHeuristicKind kind = LodHeuristicKind::Other;
    float measure = -1.0f;
    uint32_t level = 0;
    std::string reason;
    int32_t streamingHeuristic = -1;
    float streamingMeasure = -1.0f;
    uint32_t state = 0;
    std::vector<uint32_t> resident;
    std::string residentReason;
    std::vector<LodAuthoringFinding> findings;
};
using SceneLodChoices = std::unordered_map<std::string, SceneLodChoice>; // by the root's full path

// Streaming measured, per placed streaming root: which levels are ready (placed and waiting for nothing, so they can be drawn), each level's
// last lead (seconds from ready to selected, 0 when it was selected before it was ready; -1 never selected), whether it draws coarser than
// selected (late) or nothing (a hole), and how often its levels were loaded again after being released. Time is measured here, never
// acted on.
struct SceneStreamingRoot {
    std::vector<bool> ready;
    std::vector<double> lead;
    bool late = false;
    bool hole = false;
    uint32_t reloads = 0;
};

// The totals since the scene opened or view.streaming.reset: roots late and holes now; switches of the selected level to a level not ready
// (late switches), times a root started drawing nothing (holes), and the root-seconds drawn coarser than selected or nothing; levels added to roots' resident sets (loads), of them loaded
// again after a release (reloads), and taken out (released); the leads of every switch; mesh packs that arrived with no placement naming
// them any more (asked for a level no longer resident).
struct SceneStreaming {
    std::unordered_map<std::string, SceneStreamingRoot> roots;
    uint32_t lateNow = 0;
    uint32_t holesNow = 0;
    uint64_t lateSwitches = 0;
    uint64_t holes = 0;
    double lateSeconds = 0.0;
    uint64_t loads = 0;
    uint64_t reloads = 0;
    uint64_t released = 0;
    std::vector<double> leads;
    uint64_t arrivedUnwanted = 0;
    uint32_t findings = 0; // authoring findings over the placed roots, at the last evaluation
};
// The scene thread's work as of a snapshot: batches of requests and answers taken, composes (each composes with the packs held and asks for
// those it names and lacks) and placement passes (the renderer's inputs placed from the World), the assets in flight by kind, and what it holds.
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
    // Placement passes (after a compose, a relink, a level evaluation, arrivals): how many, and of the last one that changed something, its
    // time, the placements it built (new, moved, recomposed, or with an asset they waited for), and the slots it added, removed and updated.
    uint64_t places = 0;
    double lastPlaceMs = 0.0;
    uint32_t placementsBuilt = 0;
    uint32_t slotsAdded = 0;
    uint32_t slotsRemoved = 0;
    uint32_t slotsUpdated = 0;
    uint32_t placements = 0;       // in the tree, drawn or not
    uint32_t placementsDrawn = 0;  // of them, drawn
    uint64_t walksBuilt = 0;       // content walks built (new, composed again, or with a gizmo's opinion in them), since the scene opened
    uint64_t walksKept = 0;        // kept now
    uint32_t lodGroups = 0;        // render-LOD groups placed (drawn placements' render-LOD roots)
    uint32_t lodGroupsChanged = 0; // of the last pass that changed something: groups added, changed or freed
    // The LOD heuristic's last evaluation: roots evaluated, levels it changed (finer, coarser), and its time.
    uint32_t lodRootsEvaluated = 0;
    uint32_t lodFiner = 0;
    uint32_t lodCoarser = 0;
    // Unloading, since the scene opened: levels taken out of roots' resident sets, the composed contents no level reached any more, and
    // mesh packs no placement named any more.
    uint64_t releasedLevels = 0;
    uint64_t releasedContents = 0;
    uint64_t releasedMeshes = 0;
    uint64_t releasedMeshBytes = 0;
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
    bool placeNeeded = false;
    double sinceOpenMs = 0.0;
};

// What the placement tree draws, for the renderer's copy and the main thread's queries, shared by every snapshot published until the tree
// changes again. Immutable once published, but for the asset index the queries build on first use.
struct SceneDraws {
    RenderWorld drawn;
    std::shared_ptr<const Placement> placements;                               // the tree; queries walk it
    ChunkedList<std::string> primPaths;                                        // by prim index - 1; an index not in use keeps its last path
    ChunkedList<std::shared_ptr<const std::vector<std::string>>> assetsByPrim; // by prim index - 1: the assets a drawn prim's draws use
    ChunkedList<std::string> lodGroupPaths;                                    // by render-LOD group slot (RenderWorld::lodGroups): the root's full path; empty when free
    ChunkedList<std::shared_ptr<const LodGroupSource>> lodGroupSources;        // by render-LOD group slot: its root's levels and heuristics; null when free
    // Asset id to the prims whose draws use it, built from assetsByPrim on the first query.
    auto drawsByAsset() const -> const std::unordered_map<std::string, std::vector<uint32_t>>&;
    // The drawn placements' cameras, LOD roots, payload placements, point instancers and prims with variant sets, in tree order, gathered
    // from the tree on the first query.
    auto records() const -> const PlacementRecords&;
    // Each prim index's slots, built on the first query.
    auto slotsByPrim() const -> const std::vector<std::vector<uint32_t>>&;

private:
    mutable std::once_flag primIndexBuilt;
    mutable std::vector<std::vector<uint32_t>> primSlots;
    mutable std::once_flag assetIndexBuilt;
    mutable std::unordered_map<std::string, std::vector<uint32_t>> assetIndex;
    mutable std::once_flag recordsGathered;
    mutable PlacementRecords gathered;
};

// The scene as the scene thread last published it, for the main thread's queries. Immutable once published.
// The scene's sky as placed (the first dome light), for the `environment` record and the Scene's readers: what the dome authors, its image,
// and once the image is held, what the renderer lights with.
struct SceneEnvironment {
    std::string prim;
    std::string texture; // asset id
    std::string textureFormat;
    std::string poleAxis;
    glm::vec3 color = glm::vec3(1.0f);
    float intensity = 1.0f;
    float exposure = 0.0f;
    bool imageHeld = false; // its pack arrived, as an HDR lat-long with its irradiance
    uint32_t width = 0;
    uint32_t height = 0;
    std::optional<RenderEnvironment> placed; // RenderWorld::environment
    uint32_t domesIgnored = 0;               // dome lights after it, or that can't be a sky (DomeLightIgnored)
};

struct SceneSnapshot {
    std::string id;
    uint64_t revision = 0;
    std::shared_ptr<const SceneDraws> draws = std::make_shared<SceneDraws>();
    std::shared_ptr<const SceneLodChoices> lodChoices = std::make_shared<SceneLodChoices>();
    bool lodHeuristic = true;
    LodPreference lodPreference = LodPreference::Distance;
    std::shared_ptr<const LodTuning> lodTuning = std::make_shared<LodTuning>();
    // Shared between snapshots until it changes, as are the asset ids, the layer stack and the World's LOD levels below: a snapshot published
    // for a camera move copies none of them, and freeing it frees none of them.
    std::shared_ptr<const std::unordered_map<std::string, uint32_t>> lodHandLevels = std::make_shared<std::unordered_map<std::string, uint32_t>>();
    double metersPerUnitOfLod = 1.0; // the stage's, for the measures the records show
    std::shared_ptr<const SceneStreaming> streaming = std::make_shared<SceneStreaming>();
    glm::mat4 unitScale = glm::mat4(1.0f);
    // Every asset the scene uses: layer packs, meshes, materials, textures, failed ones.
    std::shared_ptr<const std::vector<std::string>> assetIds = std::make_shared<std::vector<std::string>>();
    // The World, summarised.
    std::shared_ptr<const std::vector<std::string>> layerStack = std::make_shared<std::vector<std::string>>();
    std::string upAxis = "Y";
    double metersPerUnit = 0.01;
    size_t prims = 0;
    size_t prototypes = 0;
    std::shared_ptr<const std::unordered_map<std::string, uint32_t>> lodLevels = std::make_shared<std::unordered_map<std::string, uint32_t>>();
    // Nothing queued, requested or waiting to be placed when it was published.
    bool idle = true;
    SceneThreadStats thread;
    std::optional<SceneEnvironment> environment;
};

// The view's configuration values (src/config/README.md) for the scene: which purposes beyond the default it loads and draws.
namespace viewconfig {
extern config::Bool purposeGuide; // scene.purposes.guide
extern config::Bool purposeProxy; // scene.purposes.proxy
} // namespace viewconfig

// The scene ngen-view shows: its World, composed from the scene's packs, and the renderer's inputs placed from it: a tree of placements of
// the World's contents (placement.h), whose draws are RenderWorld's slots (one per prim submesh), and the meshes, materials and textures they
// use, each kept by asset. Everything comes from the asset server; nothing of USD. See src/view/README.md.
//
// Nothing here waits. A scene thread owns the composer, the packs and the libraries: it takes requests (open, LOD, purposes, refresh,
// transforms) and the assets that arrive, composes and places per batch, and publishes a SceneSnapshot and what changed for the
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
    // Which heuristic a root measures by when it names more than one: distance first (the default), screen size first, or the first it names.
    auto setLodPreference(LodPreference preference) -> void;
    auto lodPreference() const -> LodPreference { return current->lodPreference; }
    auto lodMetersPerUnit() const -> double { return current->metersPerUnitOfLod; }
    // The LOD tuning (LodTuning): each value given is set; the others are kept. A bias at or below 0 or a hysteresis outside [0, 0.9] is
    // clamped.
    auto setLodTuning(std::optional<float> bias, std::optional<float> hysteresis, std::optional<bool> blending) -> void;
    // Sets one heuristic's thresholds for the authored root of the LOD root at a path (every placement of it), the heuristic the
    // preference uses when none is named; the lists not given keep what they had. A path with no LOD root placed is reported in the trace.
    auto overrideLodThresholds(const std::string& rootPath, std::optional<uint32_t> heuristic, LodOverride thresholds) -> void;
    // Drops every override of the authored root of the LOD root at a path; an empty path drops them all.
    auto resetLodOverrides(const std::string& rootPath) -> void;
    auto lodTuning() const -> const LodTuning& { return *current->lodTuning; }
    // The levels set by hand, by LOD root path (streaming roots and render-LOD groups alike).
    auto lodHandLevels() const -> const std::unordered_map<std::string, uint32_t>& { return *current->lodHandLevels; }
    // Streaming measured (SceneStreaming), as of the snapshot update() took; resetStreamingCounters zeroes the totals, for a run measured
    // from a known start.
    auto streaming() const -> const SceneStreaming& { return *current->streaming; }
    auto resetStreamingCounters() -> void;
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
    // True when the scene thread has nothing queued, requested or unplaced, as of the snapshot update() took and the requests since.
    auto idle() const -> bool;
    // What changed in the renderer's inputs since the last call: the first call after open has everything. Thread-safe.
    auto takeChanges() -> SceneChanges;

    // Queries over the snapshot update() took.
    auto sceneId() const -> const std::string& { return current->id; }
    auto snapshot() const -> const SceneSnapshot& { return *current; }
    auto renderWorld() const -> const RenderWorld& { return current->draws->drawn; }
    // The cameras the scene authors, in visit order.
    auto cameras() const -> const std::vector<SceneCamera>& { return current->draws->records().cameras; }
    // The scene thread's work as of the snapshot update() took, and the requests posted and applied now.
    auto threadStats() const -> const SceneThreadStats& { return current->thread; }
    auto requestCounts() const -> std::pair<uint64_t, uint64_t> { return {requestsPosted.load(), requestsApplied.load()}; }
    // The payload placements reached while drawing, in visit order.
    auto payloads() const -> const std::vector<ScenePayload>& { return current->draws->records().payloads; }
    // The LOD roots reached while drawing, in visit order.
    auto lodRoots() const -> const std::vector<SceneLodRoot>& { return current->draws->records().lodRoots; }
    auto instancers() const -> const std::vector<SceneInstancer>& { return current->draws->records().instancers; }
    auto variantPrims() const -> const std::vector<SceneVariantPrim>& { return current->draws->records().variantPrims; }
    // Moves on each time the renderer's inputs change.
    auto revision() const -> uint64_t { return current->revision; }
    // Every asset the scene uses: its layer packs, meshes, materials and textures.
    auto assetIds() const -> std::vector<std::string> { return *current->assetIds; }
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
        std::optional<LodPreference> prefer;
    };
    struct StreamingResetRequest {};
    struct TuningRequest {
        std::optional<float> bias;
        std::optional<float> hysteresis;
        std::optional<bool> blending;
        // An override (or with `reset`, dropping them) for the authored root at this path; none when empty and not reset.
        std::string path;
        std::optional<uint32_t> heuristic;
        std::optional<LodOverride> thresholds;
        bool reset = false;
    };
    using Request = std::variant<OpenRequest, LodRequest, PurposeRequest, RefreshRequest, TransformRequest, HeuristicRequest, TuningRequest, StreamingResetRequest>;
    struct CameraInput {
        glm::vec3 position = glm::vec3(0.0f);
        float fovY = 0.785f;
        float aspect = 1.0f;
    };
    // The LOD heuristic over the roots the last placement pass drew: each one's level from its imaging heuristic, with hysteresis, unless
    // set by hand, and its resident levels from its streaming heuristic; the composer gets what changed. True when something did.
    auto evaluateLevels() -> bool;
    // Streaming measured from the placed roots' rows after a pass (SceneStreaming).
    auto measureStreaming() -> void;
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
    auto releaseMeshPack(const MeshPack& pack) -> void; // counts it in the stats; the caller erases it
    auto reportAssets() -> void;
    auto publish() -> void;
    auto drawsPurpose(Purpose purpose) const -> bool;
    // A bound material (its key, World's materialKey): its pack, when held, and its textures with its inputs' values applied (a texture
    // whose file comes from an input takes the input's value where the material is used).
    struct ResolvedMaterial {
        const MaterialPack* pack = nullptr;
        std::string packId;
        std::string baseColorTexture;
        std::string opacityTexture;
    };
    auto resolveMaterial(const std::string& key) const -> ResolvedMaterial;
    auto material(const std::string& materialAsset, const std::optional<std::array<float, 3>>& displayColor) -> MaterialHandle;
    // The library entry for a texture asset, or for a constant colour's generated 1x1; added on first use.
    auto texture(const std::string& textureAsset) -> TextureHandle;
    auto constantTexture(const glm::vec4& color) -> TextureHandle;
    // The library entry for a mesh asset, added on first use; null when its pack hasn't arrived.
    auto mesh(const std::string& meshAsset) -> MeshHandle;
    // Records what the placements use (the slots' entries and those undrawn placements hold), against what they used, as changes for the
    // renderer; what isn't used any more leaves the libraries at once.
    auto collectChanges(const std::set<uint32_t>& meshesNow, const std::set<uint32_t>& materialsNow, const std::set<uint32_t>& texturesNow, SceneChanges& changes) -> void;
    auto addChanges(SceneChanges changes) -> void;
    // ContentComposed per content the last compose or relink completed.
    auto traceCompleted() -> void;

    AssetClient& assets;

    // Between the threads.
    mutable std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<Request> requests;
    bool answered = false;                          // the asset client answered something since the scene thread last looked
    bool working = false;                           // the scene thread took a batch and hasn't published its result yet
    std::shared_ptr<const SceneSnapshot> published; // the latest, for update()
    // Snapshots update() let go of: the scene thread frees them (their placements and lists only they held can take a while), so the main
    // thread only swaps a pointer.
    std::vector<std::shared_ptr<const SceneSnapshot>> retired;
    SceneChanges pending; // what changed since takeChanges() last ran
    std::atomic<uint64_t> requestsPosted{0};
    std::atomic<uint64_t> requestsApplied{0};

    // The main thread's.
    std::shared_ptr<const SceneSnapshot> current = std::make_shared<SceneSnapshot>();
    std::set<Purpose> shownPurposes = {Purpose::Default, Purpose::Render};
    mutable PlacementBounds placementBounds; // bounds(): each placement's, kept from one snapshot to the next

    // The scene thread's, from here on.
    std::string id;
    std::unique_ptr<WorldComposer> composer;
    std::set<Purpose> drawnPurposes = {Purpose::Default, Purpose::Render};
    std::set<std::string> requestedPacks;
    std::unordered_map<std::string, AssetKind> inFlight; // requested and not answered yet
    std::set<std::string> refreshing;                    // asked to be fetched again: their arrival replaces what the scene holds
    bool composeNeeded = false;
    bool placeNeeded = false;
    // The LOD heuristic: the camera the main thread posted (between the threads) and the scene thread's, the levels set by hand, what the
    // heuristic chose last per root (its hysteresis), what the composer was given, and whether to evaluate again.
    std::optional<CameraInput> postedCamera;
    std::optional<CameraInput> camera;
    bool heuristicOn = true;
    LodPreference lodPreferenceNow = LodPreference::Distance;
    std::shared_ptr<const LodTuning> tuning = std::make_shared<LodTuning>();
    auto applyTuning(const TuningRequest& request) -> void;
    // The authored root (lodRootKey) of the LOD root at a path, and its heuristics: a render-LOD group's or a placed streaming root's.
    auto authoredRootOf(const std::string& path) const -> std::optional<std::pair<std::string, const std::vector<LodHeuristic>*>>;
    bool evaluateNeeded = false;
    std::unordered_map<std::string, uint32_t> handLevels;
    std::unordered_map<std::string, uint32_t> heuristicLevels;
    std::unordered_map<std::string, uint32_t> composerLevels;
    // Residency: per placed root its streaming state (the hysteresis) and the levels the composer was told to keep loaded; both go when the
    // root is no longer placed, so a root placed again starts afresh.
    std::unordered_map<std::string, uint32_t> streamingStates;
    std::unordered_map<std::string, std::set<uint32_t>> residentSets;
    // Streaming measured: the published totals' working copy, per root when each level became ready and since when it draws late, and every
    // level released, by root path, to count a reload.
    SceneStreaming streamingNow;
    struct RootTimes {
        std::vector<std::optional<std::chrono::steady_clock::time_point>> readySince;
        std::optional<std::chrono::steady_clock::time_point> lateSince;
        std::optional<uint32_t> selected;
    };
    std::unordered_map<std::string, RootTimes> rootTimes;
    std::unordered_map<std::string, std::set<uint32_t>> releasedOf;
    bool streamingChanged = true;
    std::shared_ptr<const SceneStreaming> streamingPublished = std::make_shared<SceneStreaming>();
    uint64_t reportedReleasedLevels = 0;
    uint64_t reportedReleasedContents = 0;
    uint64_t reportedReleasedMeshes = 0;
    uint64_t reportedReleasedBytes = 0;
    // One LevelsUnloaded per batch that let something go.
    auto reportReleases() -> void;
    std::shared_ptr<const SceneLodChoices> choices = std::make_shared<SceneLodChoices>();
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

    // The renderer's inputs. The libraries keep their entries across passes; a placement takes the lowest free slot and prim index for each
    // draw it adds, and frees them when it goes.
    RenderWorld drawn;
    MeshLibrary meshLibrary;
    MaterialLibrary materialLibrary;
    TextureLibrary textureLibrary;
    std::set<uint32_t> usedMeshes; // what the slots used at the last library check, by handle
    std::set<uint32_t> usedMaterials;
    std::set<uint32_t> usedTextures;
    std::unordered_map<std::string, MaterialHandle> materialByAsset; // this pass's, by material asset id or "displayColor r g b"
    LowestFreeList freeSlots;
    LowestFreeList freePrims;
    ChunkedList<std::string> primPaths;
    ChunkedList<std::shared_ptr<const std::vector<std::string>>> assetsByPrim;
    // Kept as placements change, so no pass walks every slot or placement: the distant lights by prim index; the slots drawing each mesh
    // and material handle; the placements using each walk, and the placements naming each mesh asset (through their walks), whose packs are
    // held; the placements of each content (a placed content composed again makes the pass look at every placement); and the placements.
    std::map<uint32_t, RenderLight> lightsByPrim;
    // The dome lights placed, by prim index: the first, in prim order, is the scene's environment.
    struct PlacedDome {
        Light light;
        glm::mat4 worldTransform = glm::mat4(1.0f); // world metres
    };
    std::map<uint32_t, PlacedDome> domesByPrim;
    std::set<uint32_t> domesWarned; // the domes DomeLightIgnored was traced for, so a pass doesn't trace it again
    bool lightsChanged = false;
    std::vector<uint32_t> meshUses; // by handle index: slots drawing it and undrawn placements holding it
    std::vector<uint32_t> materialUses;
    static auto countUse(std::vector<uint32_t>& uses, uint32_t index, int delta) -> void;
    std::unordered_map<const ContentWalk*, uint32_t> walkUses;
    std::unordered_map<std::string, uint32_t> meshNames;
    std::unordered_map<int64_t, uint32_t> contentPlacements;
    // The streaming roots in drawn placements, by path, with their rows as the placements have them: what the heuristic evaluates, kept
    // as placements change so an evaluation doesn't walk the tree. `uses` counts the placements holding the root (two while one replaces
    // the other).
    struct PlacedLodRoot {
        SceneLodRoot row;
        uint32_t uses = 0;
    };
    std::map<std::string, PlacedLodRoot> placedRoots;
    // A slot per drawn streaming root, which its levels' draws carry (RenderMeshInstance::streamRoot): taken when its levels are placed,
    // given back after a pass that left it no longer placed.
    std::unordered_map<std::string, uint32_t> streamRootSlots;
    LowestFreeList freeStreamRootSlots;
    uint32_t streamRootSlotCount = 0;
    auto streamRootSlot(const std::string& path) -> uint32_t;
    std::unordered_map<int64_t, uint64_t> placedVersions; // the version each placed content had at the last pass
    uint32_t placementCount = 0;
    uint32_t drawnPlacementCount = 0;
    auto setSlot(uint32_t slot, const RenderMeshInstance& instance) -> void;
    auto useNode(const Placement& node, int delta) -> void;
    // The roots whose wanted level or hand-set state changed since the last pass: a pass looks into the placements holding them.
    std::set<std::string> rootsToCheck;
    bool passNewRoots = false;                                                // a pass drew roots not drawn before: the heuristic evaluates them
    uint32_t defaultLightPrim = 0;                                            // the engine's light's prim index while the scene has no distant light, else 0
    std::shared_ptr<const SceneDraws> draws = std::make_shared<SceneDraws>(); // the last pass's, as published
    // Counted when the World is composed, not at every publish: a content's prims (as World::visit reaches them) kept by its version.
    size_t worldPrims = 0;
    std::unordered_map<const WorldContent*, std::pair<uint64_t, size_t>> primCounts;
    auto countPrims(const World& world, const WorldContent& content, bool rootIsPrim) -> size_t;
    auto countWorld() -> void;
    // Every asset id the scene uses, rebuilt when the sets of packs change size.
    std::shared_ptr<const std::vector<std::string>> assetIdList = std::make_shared<std::vector<std::string>>();
    size_t assetIdKey = SIZE_MAX;

    // Content walks (placement.h), kept by content, version and inherited state.
    std::unordered_map<std::string, std::shared_ptr<const ContentWalk>> walks;
    // A content's walk afresh; `overrideBase`, when set, is where it is placed, to apply the gizmo's local opinions. `root`, when given, walks
    // that entity's subtree instead (a PointInstancer's prototype), the entity's path "/".
    auto buildWalk(const WorldContent& content, bool rootIsPrim, Purpose purpose, const std::string& material, bool strong, const std::string& overrideBase, EntityId root = noEntity)
        -> std::shared_ptr<const ContentWalk>;
    // A content's kept walk, afresh when a gizmo's opinion is under where it is placed.
    auto walkOf(Placement::Kind kind, int32_t index, const WorldContent& content, Purpose purpose, const std::string& material, bool strong, const std::string& usePath, EntityId root = noEntity)
        -> std::shared_ptr<const ContentWalk>;
    auto overridesUnder(const std::string& usePath) const -> bool;

    // The placement tree (placement.h), the scene thread's, its nodes shared with the snapshots published.
    std::shared_ptr<Placement> tree;
    // What a placement is: the content, where, with what it inherits, and whether it draws.
    struct PlacementSpec {
        Placement::Kind kind = Placement::Kind::Stage;
        int32_t content = -1;
        Placement::Kind contentKind = Placement::Kind::Stage;
        EntityId instancer = noEntity;
        std::string path;
        glm::mat4 world = glm::mat4(1.0f);
        Purpose purpose = Purpose::Default;
        std::string material;
        bool strong = false;
        uint32_t lodTag = 0;
        uint32_t lodDepth = 0;
        uint32_t streamRoot = 0;
        bool drawn = true;
        uint32_t lodGroup = 0; // Placement::lodGroup
        uint32_t lodGroupLevel = 0;
    };
    // One pass over the tree after a compose, a relink, a level evaluation, arrivals or a gizmo's move: placements whose content changed,
    // that moved, or that waited for an asset that arrived are built again; LOD roots switch levels; the rest is kept as it is.
    auto place() -> void;
    // A placement as the spec says, from what it was (null for a new one): the same node when nothing changed, else a changed one (the
    // node itself when `mine`, the scene thread's alone, else a copy).
    auto syncPlacement(const std::shared_ptr<Placement>& node, const PlacementSpec& spec, bool mine) -> std::shared_ptr<Placement>;
    auto buildPlacement(const std::shared_ptr<Placement>& node, const PlacementSpec& spec, uint64_t version, bool mine) -> std::shared_ptr<Placement>;
    auto undrawPlacement(const std::shared_ptr<Placement>& node, bool mine) -> std::shared_ptr<Placement>;
    // A placement's LOD roots' levels: a placement per loaded, linked level, the drawn one drawn. `edit` gives the node to change.
    // `all`: every root (a placement just built), else those pending, changed or under a gizmo's move.
    auto syncRoots(const Placement& node, const std::function<Placement&()>& edit, bool mine, bool all) -> void;
    auto releasePlacement(const Placement& node) -> void;
    auto contentOf(const World& world, Placement::Kind kind, int32_t index) const -> const WorldContent&;
    auto allocSlot() -> uint32_t;
    auto freeSlot(uint32_t slot) -> void;
    auto allocPrim(const std::string& path) -> uint32_t;
    auto freePrim(uint32_t prim) -> void;
    auto touchSlot(uint32_t slot) -> void;
    // Render-LOD group slots (RenderWorld::lodGroups), taken and freed like draw slots, with what the view knows of each: the root's full
    // path, its default level, whether its heuristic is usable, and its level count; and by path.
    struct LodGroupInfo {
        std::string path;
        uint32_t defaultLevel = 0;
        uint32_t levelCount = 0;
        std::vector<LodHeuristic> heuristics;
        glm::mat4 toWorld = glm::mat4(1.0f); // the root's space to world metres
        std::string key;                     // its authored root (lodRootKey)
    };
    std::vector<LodGroupInfo> lodGroupInfo;
    ChunkedList<std::string> lodGroupPaths; // published with the draws
    ChunkedList<std::shared_ptr<const LodGroupSource>> lodGroupSources;
    std::unordered_map<std::string, uint32_t> lodGroupByPath;
    LowestFreeList freeLodGroups;
    TouchedList<RenderLodGroup> lodGroupsBefore;
    uint32_t lodGroupCountBefore = 0;
    auto allocLodGroup() -> uint32_t;
    auto freeLodGroup(uint32_t group) -> void;
    auto setLodGroup(uint32_t group, const RenderLodGroup& value) -> void;
    // A group's forced level: hand-set, else its default while the heuristic is off or it has no usable heuristic, else none (-1).
    auto forcedLevelOf(const LodGroupInfo& info) const -> int32_t;
    // A group's record from what the view knows of it: the heuristic the preference chooses, in the GPU's units (thresholds in metres for a
    // distance), and its forced level.
    auto lodGroupOf(const LodGroupInfo& info) const -> RenderLodGroup;
    // Builds the group at a path again, or with an empty path every group (the heuristic turned on or off, the preference changed).
    auto refreshLodGroups(const std::string& path) -> void;
    // The pass's: assets that arrived or failed since the last (a placement waiting for one is built again), the slots it touched with what
    // they held before, the assets its placements want and lack, and its counts.
    std::set<std::string> arrived;
    TouchedList<RenderMeshInstance> slotsBefore;
    uint32_t slotCountBefore = 0;
    std::vector<std::pair<std::string, AssetKind>> passWanted;
    uint32_t passBuilt = 0;
    bool passEdited = false; // a placement changed without being built again (a root drawing another level)
    uint32_t passSwitchedFiner = 0;
    uint32_t passSwitchedCoarser = 0;
    std::string passFirstSwitched;
    bool resetPlacements = false;            // the tree is released and placed afresh: a purpose shown or hidden, a mesh or material repacked
    std::vector<std::string> overridesMoved; // the gizmo's moves since the last pass: the placements holding them are built again
    // A mesh where it is drawn with a material binding: the materials of its submeshes, whether each cuts, and the assets its draws use (the
    // mesh, the file defining it, each material and its textures). Shared, this pass, by the prims drawing the same mesh and binding.
    struct ResolvedDraw {
        std::vector<MaterialHandle> materials;
        std::vector<bool> cutout;
        std::shared_ptr<const std::vector<std::string>> uses;
    };
    std::unordered_map<std::string, std::shared_ptr<const ResolvedDraw>> resolvedDraws;
    auto resolveDraw(const std::string& meshAsset, const std::string& boundMaterial, bool strongBinding, const Entity* subsetsOf, const WorldContent& content) -> std::shared_ptr<const ResolvedDraw>;
    // An asset a placement's prims want and lack, into its `waiting` and the pass's requests (once each; a failed one isn't waited for).
    auto wantAsset(Placement& node, const std::string& assetId, AssetKind kind) -> void;
    // The assets a material binding wants and lacks: its pack, or its textures once the pack is held.
    auto wantMaterial(const std::string& key, Placement& node) -> void;
    // The distant lights, or the engine's light when there is none, after a pass that changed them.
    auto collectLights() -> void;
    // The scene's environment, from its first dome light whose lat-long texture is held; none without one.
    auto collectEnvironment() -> std::optional<RenderEnvironment>;
    // Whether a pass looks into a placement at this path that isn't pending: a gizmo moved, or a root changed its wanted level, under it.
    auto touched(const std::string& path) const -> bool;
    bool fullPass = false;                           // a placed content was composed again: the pass looks into every placement
    SceneThreadStats stats;                          // the counters; publish() fills in the rest
    std::map<std::string, glm::mat4> localOverrides; // setLocalTransform's, applied by every walk under them
    glm::mat4 unitScale = glm::mat4(1.0f);
    uint64_t inputsRevision = 0;

    std::jthread thread; // last: started when everything above exists
};
