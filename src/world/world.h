#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// The runtime scene: what the engine draws, composed from scene packs by WorldComposer (worldcomposer.h). No USD in it. See
// src/world/README.md.
//
// Every composed prim is an entity, whatever its type or purpose. Instanced content is kept once, in a prototype shared by every instance
// of it; an instance entity names its prototype and has no children of its own. `visit` walks the world with instances expanded.

using EntityId = uint32_t;
inline constexpr EntityId noEntity = UINT32_MAX;

enum class Purpose : uint8_t {
    Default,
    Render,
    Proxy,
    Guide,
};

// A material binding as composed on a prim: the material it names, as the material's asset id, and whether it overrides the bindings of
// the prim's descendants (bindMaterialAs = strongerThanDescendants).
struct MaterialBinding {
    std::string targetPath; // the bound prim's path, in the namespace of the content holding the binding
    std::string materialAsset;
    bool strongerThanDescendants = false;
};

struct Entity {
    std::string name; // the last element of its path; empty for a content's root
    std::string typeName;
    EntityId parent = noEntity;
    std::vector<EntityId> children;
    bool defined = false;  // some layer defines it (def or class), not only overs
    bool abstract = false; // its strongest specifier is class
    bool active = true;
    int32_t prototype = -1; // an instance: the index of its prototype in World::contents, once the prototype is complete
    // A LOD level: a stand-in for a content of its own, the index in World::contents of the content that holds the level prim (its entity
    // 0) and everything under it, once that content is complete.
    int32_t linkedContent = -1;
    // A LOD item, a LOD root's child (its stand-in, and its content's entity 0): its authored visibility is what a reader that doesn't
    // select LOD sees; a LOD-aware reader shows the item it selects whatever it authors, as UsdLod says.
    bool lodItem = false;
    int64_t pointCount = -1;        // the composed `points` count of a point-based prim, -1 for other prims
    std::optional<Purpose> purpose; // authored; inherited from the nearest ancestor that authors one
    bool invisible = false;         // authored visibility "invisible"; hides the subtree
    bool xformable = false;         // its type takes transform operations
    bool resetsXformStack = false;  // its transform ignores its ancestors'
    glm::dmat4 localTransform = glm::dmat4(1.0);
    std::string meshAsset;     // the mesh asset its geometry is, when it is geometry the engine draws
    std::string materialAsset; // the material asset it is, when it is a material
    // A material's own inputs (its interface, inputs:*) as composed, the strongest opinion each, by name without "inputs:": the values
    // that set its network where it is used. A value is text (an asset input's asset id; numbers comma-separated).
    std::vector<std::pair<std::string, std::string>> materialInputs;
    // Its variant sets as composed: each set authored on any node of its index, and the variant selected (empty when none is).
    std::vector<std::pair<std::string, std::string>> variantSelections;
    std::optional<MaterialBinding> binding;
};

// A material as a binding names it: its pack's asset id and, when it has inputs, their values, as "<asset>?<input>=<value>&<input>=<value>"
// (inputs by name). Materials with the same pack and the same values are the same material.
auto materialKey(const Entity& material) -> std::string;
struct MaterialUse {
    std::string asset;                                       // the material pack
    std::vector<std::pair<std::string, std::string>> inputs; // its inputs' values where it is used
    auto input(const std::string& name) const -> std::optional<std::string>;
};
auto parseMaterialKey(const std::string& key) -> MaterialUse;

enum class LightShape : uint8_t {
    Distant,
    Sphere,
    Rect,
    Disk,
    Cylinder,
    Dome,
};

// UsdLux's inputs, with UsdLux's fallbacks.
struct Light {
    LightShape shape = LightShape::Distant;
    glm::vec3 color = glm::vec3(1.0f);
    float intensity = 1.0f;
    float exposure = 0.0f;
    float angle = 0.53f;
    bool shadowEnable = true;
    glm::vec3 shadowColor = glm::vec3(0.0f);
    // A dome's image: its asset id (empty for none), how it maps directions (`inputs:texture:format`: automatic, latlong, mirroredBall,
    // angular, cubeMapVerticalCross) and the axis its pole is on (DomeLight_1's `poleAxis`: scene, Y, Z; a DomeLight's is Y).
    std::string texture;
    std::string textureFormat = "automatic";
    std::string poleAxis = "scene";
};

// A prim with LODRootAPI: its children are its levels, finest first. `loaded` holds the levels whose payloads are loaded in this content;
// which level an instance wants and draws is state of the instance (World::wantedLevel, World::drawnLevel).
// How a LOD heuristic measures a root's screen size (UsdLod's projectionMethod).
enum class LodProjection : uint8_t {
    Sphere, // projectedSphere, UsdLod's fallback: the extent's bounding sphere
    Extent, // projectedExtent: the extent's box
};

// The heuristics the engine reads (UsdLod's LODDistanceHeuristic and LODScreenSizeHeuristic), and the rest, kept to show.
enum class LodHeuristicKind : uint8_t {
    Distance,
    ScreenSize,
    Other,
};

// What a heuristic decides (its lod:domain): what is drawn (UsdLod's `imaging`), what is resident (`streaming`, a domain token of the
// world's own, its thresholds the load radii and `streaming:unloadThresholds` the unload radii), or something the engine doesn't read
// (`physics`, `audio`, any other), kept to show.
enum class LodDomain : uint8_t {
    Imaging,
    Streaming,
    Other,
};

// One heuristic a LOD root names (lod:heuristics), as authored, its bounds resolved as UsdLod's queries read them, in the root's space:
// - Distance: from the viewpoint to `center`, or to the nearest point of `box` when its boundingVolume names a boundable with a valid
//   extent (0 inside it). Thresholds ascend.
// - ScreenSize: `extent` (its boundingVolume's, else its own, else UsdLod's fallback -1..1), as a sphere or a box (`projection`).
//   Thresholds descend.
// `blendThresholds` as authored (shorter or empty: no blend past its end). A streaming heuristic's `thresholds` are its load radii (or load
// screen sizes) and `unloadThresholds` its unload ones, one per load threshold on its coarser side (empty: the load ones, no hysteresis).
// `unusable` says why the engine doesn't read it, empty when it does: a type or domain it doesn't read, no thresholds, thresholds out of
// order, unload thresholds that don't match the load ones, bounds that aren't finite.
struct LodHeuristic {
    LodHeuristicKind kind = LodHeuristicKind::Other;
    std::string type; // its prim type
    std::string path; // its prim's path in the root's content
    std::string domain;
    LodDomain domainKind = LodDomain::Other;
    std::vector<float> thresholds;
    std::vector<float> blendThresholds;
    std::vector<float> unloadThresholds;
    std::array<glm::vec3, 2> extent = {glm::vec3(-1.0f), glm::vec3(1.0f)};
    LodProjection projection = LodProjection::Sphere;
    glm::vec3 center = glm::vec3(0.0f);
    std::optional<std::array<glm::vec3, 2>> box;
    std::string unusable;

    auto usable() const -> bool { return unusable.empty(); }
    auto operator==(const LodHeuristic&) const -> bool = default;
};

// Which kind a root measures by when it authors both (the view's preference), and UsdLod's order otherwise.
enum class LodPreference : uint8_t {
    Distance,   // the first usable distance heuristic, else the first usable screen-size one
    ScreenSize, // the other way round
    Authored,   // the first usable heuristic, as UsdLod has a renderer choose
};

// A preference's name, as the verb and the records say it ("distance", "screen", "authored"), and back.
auto lodPreferenceName(LodPreference preference) -> const char*;
auto lodPreferenceFromName(std::string_view name) -> std::optional<LodPreference>;

// The heuristic of a domain a root measures by under a preference: an index into its heuristics, -1 when none is usable.
auto chooseLodHeuristic(const std::vector<LodHeuristic>& heuristics, LodDomain domain, LodPreference preference) -> int32_t;

// A streaming heuristic's state for a root: the finest level that must be resident. Going finer by the load thresholds, at once; going
// coarser than `previous` only past the unload thresholds (empty: the load ones). Without a previous state, the load thresholds alone.
auto streamingState(LodHeuristicKind kind, const std::vector<float>& load, const std::vector<float>& unload, float measure, uint32_t levelCount, std::optional<uint32_t> previous) -> uint32_t;

// One rule of the streaming authoring contract a root breaks, with the values compared: every usable heuristic has a threshold per level
// boundary; per boundary, the imaging threshold is at or before the load radius and the load radius at or before the unload radius; a child
// root's imaging thresholds and unload radii lie inside its parent's first imaging threshold and first load radius. `boundary` is the level
// boundary (0: between levels 0 and 1).
struct LodAuthoringFinding {
    std::string rule;
    std::string heuristic; // the heuristic's path, or the parent's root path for a nesting rule
    uint32_t boundary = 0;
    std::string detail;

    auto operator==(const LodAuthoringFinding&) const -> bool = default;
};
// The contract checked for a root with `levelCount` levels and its heuristics; `parent`, when given, the heuristics of the streaming root
// above it, for the nesting rules. Only distance heuristics are compared across domains and roots, as the contract states them in metres.
auto checkLodAuthoring(const std::vector<LodHeuristic>& heuristics, uint32_t levelCount, const std::vector<LodHeuristic>* parent) -> std::vector<LodAuthoringFinding>;

// The level a measure picks, as UsdLod's ComputeLOD with its blend bands, and with hysteresis at a boundary without a band: going coarser
// waits until the measure is past the threshold by `hysteresis` (a screen size below threshold * (1 - hysteresis), a distance beyond
// threshold / (1 - hysteresis) ^ 0.5, the same margin in a size that falls with the square of the distance), against `previous`, the level
// wanted last. Inside a band the level is the finer one and `blend` the share of the next (0 to 1, as UsdLod's fractional index).
struct LodPick {
    uint32_t level = 0;
    float blend = 0.0f;
};
auto pickLodLevel(LodHeuristicKind kind, const std::vector<float>& thresholds, const std::vector<float>& blendThresholds, float measure, uint32_t levelCount, uint32_t previous, float hysteresis) -> LodPick;

struct LodRoot {
    std::vector<EntityId> levels;
    // A level is a payload (any of them): the root streams, each level a content of its own linked as it loads, the level chosen per root.
    // Else its levels are render LOD: composed in place with the root's content, every one loaded, the level chosen per placement by the
    // renderer.
    bool streams = true;
    uint32_t defaultIndex = 0; // lod:default:index, clamped to the levels
    std::vector<uint32_t> loaded;
    // Its lod:heuristics targets found in this content, in authored order; which one measures it is the reader's (chooseLodHeuristic).
    std::vector<LodHeuristic> heuristics;
};

// A UsdGeomPointInstancer: its prototypes (the prims its `prototypes` relationship names, in this content; noEntity for one that isn't
// here) and per point which prototype, where in the instancer's space (translate * rotate * scale from positions, orientations and
// scales, as UsdGeomPointInstancer composes them, the prototype's own transform not included) and whether it is shown (invisibleIds).
// Arrays whose lengths don't match protoIndices make no points, as in USD.
struct PointInstancerData {
    std::vector<EntityId> prototypes;
    std::vector<uint32_t> protoIndices;
    std::vector<glm::dmat4> transforms; // in double, as entity transforms and USD's are
    std::vector<int64_t> ids;           // the authored ids; empty when none are, a point's id then being its index
    std::vector<uint8_t> visible;
};

// A prim with payloads: what they are, whether they are loaded, and the bounds authored for them (extentsHint).
struct PayloadPlacement {
    std::vector<std::string> assetIds;
    bool loaded = false;
    std::optional<std::array<glm::vec3, 2>> extentsHint;
};

// What a content is: the stage; a prototype, whose entity 0 stands for every instance (its children are every instance's children); or a
// LOD level, whose entity 0 is the level prim.
enum class ContentKind : uint8_t {
    Stage,
    Prototype,
    Level,
};

// One composed namespace: the stage, a prototype or a LOD level. Entity 0 is its root: the pseudo-root "/" for the stage, the instance
// prim for a prototype, the level prim for a level. Paths are relative to the content: a prototype's children are "/<name>". Every content
// other than the stage is complete (composed with every pack it named held, or failed) once anything links to it.
struct WorldContent {
    ContentKind kind = ContentKind::Stage;
    std::string sourcePath; // the stage's "/"; for a prototype, the first instance's path (for messages)
    uint64_t version = 0;   // which composition this is: a content composed again (or the stage, every compose) gets a new one
    std::vector<Entity> entities;
    std::unordered_map<std::string, EntityId> byPath;
    std::unordered_map<EntityId, Light> lights;
    std::unordered_map<EntityId, LodRoot> lodRoots;
    std::unordered_map<EntityId, PayloadPlacement> payloads;
    std::unordered_map<EntityId, PointInstancerData> pointInstancers;

    auto path(EntityId id) const -> std::string;
    // A prim a PointInstancer in this content names as a prototype and has under it: drawn only through the instancer, as UsdImaging
    // draws prototypes beneath their instancer; the prim and what is under it are still entities.
    auto isInstancerPrototype(EntityId id) const -> bool;
};

// An entity as `visit` reaches it: its full path with instances expanded, and what it inherits.
struct VisitedEntity {
    const WorldContent* content = nullptr;
    EntityId id = noEntity;
    const Entity* entity = nullptr;
    std::string path;
    glm::dmat4 worldTransform = glm::dmat4(1.0);
    bool visible = true;
    Purpose purpose = Purpose::Default;
    std::string materialAsset;  // the material bound to it, through its ancestors' bindings
    bool strongBinding = false; // that binding is an ancestor's strongerThanDescendants one, which a child's own binding doesn't override
    bool insideInstance = false;
    // A LOD level the instance doesn't draw: its drawn level is another of its root's levels.
    bool inUndrawnLevel = false;
    // Under a PointInstancer's prototype beneath the instancer: drawn per point, not where it is.
    bool inInstancerPrototype = false;
};

struct World {
    WorldContent stage;
    // Prototypes and LOD levels, kept across composes at stable indices (an instance's prototype, a level stand-in's linkedContent).
    std::deque<WorldContent> contents;
    std::string upAxis = "Y";            // the root layer's, USD's fallback Y when it authors none
    double metersPerUnit = 0.01;         // the root layer's, USD's fallback 0.01 when it authors none
    std::vector<std::string> layerStack; // the root layer stack's asset ids, strongest first
    // Levels set by hand, by the LOD root's full path (instances expanded); every other root wants its lod:default:index.
    std::unordered_map<std::string, uint32_t> lodLevels;

    // The level a LOD root wants: set by hand for its path, or its default.
    auto wantedLevel(const std::string& rootPath, const LodRoot& root) const -> uint32_t;
    // The level it draws: the finest loaded level not finer than the wanted one, else the finest loaded one.
    auto drawnLevel(const std::string& rootPath, const LodRoot& root) const -> std::optional<uint32_t>;

    // The prototypes held in contents.
    auto prototypeCount() const -> size_t;
    // Every entity, depth first, parents before children, instances expanded; the stage's root is not visited. Returning false from
    // `visitor` skips the entity's children.
    auto visit(const std::function<bool(const VisitedEntity&)>& visitor) const -> void;
};
