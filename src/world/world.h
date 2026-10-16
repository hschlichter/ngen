#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
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
    int32_t prototype = -1; // an instance: the index of its prototype in World::prototypes
    // A LOD level, or a prim with payloads: a stand-in for a content of its own, the index in World::linkedContents of the content that
    // holds the prim itself (its entity 0) and everything under it.
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
};

// A prim with LODRootAPI: its children are its levels, finest first. `loaded` holds the levels whose payloads are loaded in this content;
// which level an instance wants and draws is state of the instance (World::wantedLevel, World::drawnLevel).
// How a LOD heuristic measures a root's screen size (UsdLod's projectionMethod).
enum class LodProjection : uint8_t {
    Sphere, // projectedSphere, UsdLod's fallback: the extent's bounding sphere
    Extent, // projectedExtent: the extent's box
};

struct LodRoot {
    std::vector<EntityId> levels;
    uint32_t defaultIndex = 0; // lod:default:index, clamped to the levels
    std::vector<uint32_t> loaded;
    // The heuristic chosen from its lod:heuristics targets in this content: the first screen-size heuristic in the imaging domain with
    // valid bounds (heuristicChosen), else the first target, reported (heuristicType says which) while the root keeps its default.
    EntityId heuristic = noEntity;
    bool heuristicChosen = false;
    std::string heuristicType;     // its prim type: LODScreenSizeHeuristic, LODDistanceHeuristic, or another
    std::vector<float> thresholds; // the heuristic's
    // The screen-size heuristic's extent, in the root's local space: its boundingVolume's when that names a boundable with a valid
    // extent, else its own extent attribute, else UsdLod's fallback (-1..1).
    std::array<glm::vec3, 2> extent = {glm::vec3(-1.0f), glm::vec3(1.0f)};
    LodProjection projection = LodProjection::Sphere;
};

// A UsdGeomPointInstancer: its prototypes (the prims its `prototypes` relationship names, in this content; noEntity for one that isn't
// here) and per point which prototype, where in the instancer's space (translate * rotate * scale from positions, orientations and
// scales, as UsdGeomPointInstancer composes them, the prototype's own transform not included) and whether it is shown (invisibleIds).
// Arrays whose lengths don't match protoIndices make no points, as in USD.
struct PointInstancerData {
    std::vector<EntityId> prototypes;
    std::vector<uint32_t> protoIndices;
    std::vector<glm::dmat4> transforms; // in double, as entity transforms and USD's are
    std::vector<int64_t> ids; // the authored ids; empty when none are, a point's id then being its index
    std::vector<uint8_t> visible;
};

// A prim with payloads: what they are, whether they are loaded, and the bounds authored for them (extentsHint).
struct PayloadPlacement {
    std::vector<std::string> assetIds;
    bool loaded = false;
    std::optional<std::array<glm::vec3, 2>> extentsHint;
};

// One composed namespace: the stage, or a prototype. Entity 0 is its root: the pseudo-root "/" for the stage, the instance prim for a
// prototype (its children are every instance's children). Paths are relative to the content: a prototype's children are "/<name>".
struct WorldContent {
    std::string sourcePath; // the stage's "/"; for a prototype, the first instance's path (for messages)
    uint64_t version = 0;   // which composition this is: a content composed again (or the stage, every compose) gets a new one
    bool complete = false;  // composed with every pack it named held (or failed)
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
    // Kept across composes, so an index stays the same content: prototypes, and linked contents (a LOD level as loaded or not, a prim with
    // payloads), linked to their stand-in entity by linkedContent.
    std::deque<WorldContent> prototypes;
    std::deque<WorldContent> linkedContents;
    std::string upAxis = "Y";            // the root layer's, USD's fallback Y when it authors none
    double metersPerUnit = 0.01;         // the root layer's, USD's fallback 0.01 when it authors none
    std::vector<std::string> layerStack; // the root layer stack's asset ids, strongest first
    // Levels set by hand, by the LOD root's full path (instances expanded); every other root wants its lod:default:index.
    std::unordered_map<std::string, uint32_t> lodLevels;

    // The level a LOD root wants: set by hand for its path, or its default.
    auto wantedLevel(const std::string& rootPath, const LodRoot& root) const -> uint32_t;
    // The level it draws: the finest loaded level not finer than the wanted one, else the finest loaded one.
    auto drawnLevel(const std::string& rootPath, const LodRoot& root) const -> std::optional<uint32_t>;

    // Every entity, depth first, parents before children, instances expanded; the stage's root is not visited. Returning false from
    // `visitor` skips the entity's children.
    auto visit(const std::function<bool(const VisitedEntity&)>& visitor) const -> void;
};
