#pragma once

#include "renderworld.h"
#include "world.h"

#include <glm/glm.hpp>

#include <array>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// A prim's transforms as the view draws it: local, and world before the unit scale.
struct EntityTransform {
    glm::mat4 local = glm::mat4(1.0f);
    glm::mat4 world = glm::mat4(1.0f);
    bool xformable = false;
    bool resetsXformStack = false;
    bool absolute = false; // relative to a content's root: a !resetXformStack! in it, so where the content is placed doesn't apply
};

// A content's prims' transforms relative to its root, by path relative to the root.
using ContentTransforms = std::unordered_map<std::string, EntityTransform>;

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

// A payload placement reached while drawing: the prim with payloads, the asset ids, whether they are loaded, and the bounds authored for it
// (extentsHint) in world metres; none when it authors none.
struct ScenePayload {
    std::string path;
    std::vector<std::string> assetIds;
    bool loaded = false;
    std::optional<AABB> worldBounds;
};

// A PointInstancer reached while drawing: its full path, its points and those shown, its prototypes' full paths, and the prims its shown
// points draw (each point draws its prototype's drawn prims).
struct SceneInstancer {
    std::string path;
    uint32_t points = 0;
    uint32_t visiblePoints = 0;
    std::vector<std::string> prototypes;
    uint64_t primsDrawn = 0;
};

// A prim reached while drawing that has variant sets: its full path, and each set with the variant it composed to (empty for none).
struct SceneVariantPrim {
    std::string path;
    std::vector<std::pair<std::string, std::string>> selections;
};

// What a placement takes from a content: its prims that draw, light, frame (a camera), root LOD, have variant sets or place points, in walk
// order, with transforms relative to the content's root, and where its instances and levels sit. Built once per content version and the state
// the content inherits where it is placed (purpose and material binding); built afresh where a gizmo's opinion is under the placement.
struct WalkItem {
    enum class Kind : uint8_t {
        Prim,     // a prim that draws, lights, frames, roots LOD, has variant sets or places payloads
        Instance, // an instance: its prototype is placed here
        Level,    // one level of a LOD root in the content (the root's item is before it)
        // A PointInstancer: each shown point places its prototype's walk (a walk rooted at the prototype, in the same content) at the
        // instancer's world transform times the point's.
        PointInstancer,
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
    int32_t content = -1; // an Instance's prototype
    bool camera = false;
    bool payload = false;
    bool lodRoot = false;
    bool light = false;
    bool variants = false;    // it has variant sets (the `variants` record)
    bool subsets = false;     // its mesh's GeomSubsets may bind their own materials
    EntityId root = noEntity; // a Level's LOD root, in the content
    uint32_t level = 0;
};

struct ContentWalk {
    uint64_t version = 0;
    std::vector<WalkItem> items;
    std::shared_ptr<const ContentTransforms> transforms;
    std::vector<uint32_t> meshItems;     // the items that draw a mesh
    std::vector<std::string> meshAssets; // their meshes, each once: a walk outlives the composition its entities point into
    std::vector<uint32_t> rowItems;      // the items a record lists: cameras, prims with variant sets or payloads, LOD roots
};

// A walk where a placement uses it: at a full path and a world transform (before the unit scale). A placement has one; a PointInstancer's
// placement has one per shown point, of that point's prototype's walk.
struct PlacementSegment {
    std::shared_ptr<const ContentWalk> walk;
    std::string path;
    glm::mat4 world = glm::mat4(1.0f);
};

struct Placement;

// A LOD root where a placement uses it: the level it draws, and a placement per loaded level (by level index; none for a level not loaded
// or not linked yet). Only the drawn level's placement owns slots. Pending while it doesn't draw the level it wants.
struct PlacedRoot {
    uint32_t segment = 0;
    uint32_t item = 0; // the root's item in the segment's walk
    std::string path;
    std::optional<uint32_t> drawn;
    bool pending = false;
    bool listed = false; // in the `lod` record and the heuristic's: not a root in a point's prototype
    SceneLodRoot row;
    std::vector<std::shared_ptr<Placement>> levels;
};

// A content placed in the world: the stage, a prototype at an instance, a LOD level at its root, or a PointInstancer's points. It owns the
// draws of its own prims (slots in RenderWorld::meshInstances, prim indices for their paths) while drawn, and its children: the instances and
// point instancers in it, and its LOD roots' levels. Shared with published snapshots: the scene thread copies a node before it changes one
// another snapshot can see.
struct Placement {
    enum class Kind : uint8_t {
        Stage,
        Prototype,
        Level,
        Instancer,
    };
    Kind kind = Kind::Stage;
    int32_t content = -1;              // in World::contents; the stage's -1; an Instancer's, the content holding the PointInstancer
    Kind contentKind = Kind::Stage;    // an Instancer's content's kind
    EntityId instancer = noEntity;     // an Instancer's PointInstancer, in its content
    uint64_t version = 0;              // the content's, as placed
    std::string path;                  // full path of the content's root prim ("/" for the stage)
    glm::mat4 world = glm::mat4(1.0f); // before the unit scale
    Purpose purpose = Purpose::Default;
    std::string material;
    bool strong = false;
    uint32_t lodTag = 0; // the drawn level + 1 of the nearest LOD root above, for the draws
    bool drawn = true;   // false: a loaded level not drawn (the floor under a finer one, a finer one loading), and everything in it
    std::vector<PlacementSegment> segments;
    std::vector<uint32_t> slots;      // its draws
    std::vector<uint32_t> prims;      // prim indices of its drawn prims and lights
    std::vector<RenderLight> lights;  // its distant lights
    std::vector<std::string> waiting; // assets its prims want that aren't held: meshes, material packs, textures
    uint32_t pending = 0;             // waiting assets and pending roots, in it and below: a pass looks into it while not 0
    std::vector<std::shared_ptr<Placement>> children;
    std::vector<PlacedRoot> roots;
    // What the records list of it while drawn.
    std::vector<SceneCamera> cameras;
    std::vector<ScenePayload> payloads;
    std::vector<SceneVariantPrim> variantPrims;
    std::optional<SceneInstancer> instancerRow; // an Instancer's, without primsDrawn (counted when the records are gathered)
};

// The records of a tree's drawn placements, in tree order.
struct PlacementRecords {
    std::vector<SceneCamera> cameras;
    std::vector<SceneLodRoot> lodRoots;
    std::vector<ScenePayload> payloads;
    std::vector<SceneInstancer> instancers;
    std::vector<SceneVariantPrim> variantPrims;
};
auto gatherRecords(const Placement& tree) -> PlacementRecords;

// A prim's transform in a published tree, world before the unit scale; none when no drawn placement holds it (inactive, invisible, in a
// level not drawn).
auto findTransform(const Placement& tree, const std::string& path) -> std::optional<EntityTransform>;
