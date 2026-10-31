#pragma once

#include "config.h"

#include "mesh.h"
#include "renderworld.h"
#include "shadowcascades.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class DebugDraw;

// The camera modes' state, for the camera that has the controls: following the ground (its eye kept a height above the render geometry
// under it; on and the height are configuration values, `view.camera.follow*`) and orbiting (Left Alt held: right-drag turns around the
// surface under the cursor, the wheel moves toward it). ngen-view fills it in each frame (src/view/README.md, "Camera modes").
struct CameraModesState {
    // The last ground test: what it hit (none: no drawn surface under the camera, the height held), and what it tested.
    std::optional<glm::vec3> ground;
    std::string groundPrim;
    uint32_t groundChunks = 0;
    uint32_t groundDraws = 0;
    uint64_t groundTriangles = 0;
    float groundMs = 0.0f;
    // Orbiting: the pivot, from the depth under the cursor when Alt went down (a few frames later), and until then the point at the last
    // pivot's distance along the cursor ray.
    bool orbiting = false;
    glm::vec3 pivot = glm::vec3(0.0f);
    bool pivotPicked = false;
    float pivotDistanceMetres = 10.0f;
};

// The ground under a camera, for following it (src/view/README.md, "Camera modes"): the render geometry the main thread holds, tested
// naively: the chunks of draws whose bounds (SceneBoundsCache, kept for the shadow cascades) hold the camera across the up axis, in them the
// draws whose bounds do, every triangle of theirs.

struct GroundHit {
    glm::vec3 point = glm::vec3(0.0f); // world
    uint32_t prim = 0;                 // ViewScene::primPath index of the draw it hit
};

struct GroundTestCounts {
    uint32_t chunks = 0;    // chunks of draws whose bounds held the camera
    uint32_t draws = 0;     // draws whose bounds held the camera
    uint64_t triangles = 0; // their triangles, tested
};

// The meshes the draws use, by MeshHandle index, kept from the scene's changes as the main thread passes them to the renderer.
class GroundMeshes {
public:
    auto apply(const SceneChanges& changes) -> void;
    auto get(MeshHandle handle) const -> const MeshDesc*;

private:
    std::vector<std::shared_ptr<const MeshDesc>> meshes;
};

// The highest surface below `origin` along -up: the triangles of every alive draw whose world bounds hold `origin` across the up axis, in
// the chunks whose bounds (`chunkBounds`, updated for `world` this frame) do.
auto groundBelow(const RenderWorld& world, const SceneBoundsCache& chunkBounds, const GroundMeshes& meshes, const glm::vec3& origin, const glm::vec3& up, GroundTestCounts& counts)
    -> std::optional<GroundHit>;

// The modes' overlays: the orbit's pivot as a cross while orbiting; while following, a line from the eye down to the ground hit and a cross
// at it.
auto drawCameraModes(const CameraModesState& modes, const glm::vec3& eye, double metersPerUnit, DebugDraw& draw) -> void;

// The camera modes' configuration values (src/config/README.md), for the camera that has the controls.
namespace viewconfig {
extern config::Bool follow;        // view.camera.follow
extern config::Float followHeight; // view.camera.followHeight, metres
} // namespace viewconfig
