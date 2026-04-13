#pragma once

#include "boundscache.h"
#include "scenehandles.h"
#include "scenetypes.h"
#include "spatialindex.h"

#include <span>
#include <vector>

class USDScene;
class MeshLibrary;

struct RaycastHit {
    PrimHandle prim;
    float distance = 0.0f;
    glm::vec3 position;
    glm::vec3 normal;
};

class SceneQuerySystem {
public:
    void rebuild(const USDScene& scene, const MeshLibrary& meshLib);
    void updateDirty(const USDScene& scene, const MeshLibrary& meshLib, std::span<const PrimHandle> dirtyPrims, uint32_t frame);

    bool raycast(const Ray& ray, float maxDistance, RaycastHit& outHit) const;
    void frustumCull(const Frustum& frustum, std::vector<PrimHandle>& outPrims) const;

    // World-space union of bounds across `prim` and all renderable descendants.
    // Returns an invalid AABB when nothing in the subtree has bounds (e.g. an
    // empty Xform). Used for gizmo anchoring and "frame selected" so picking
    // a parent xform behaves intuitively instead of snapping to its pivot.
    AABB subtreeBounds(const USDScene& scene, PrimHandle prim) const;

    // Best bounds to anchor a gizmo / frame action on for `prim`. Prefers the
    // prim's own non-degenerate bounds (so a renderable leaf anchors on itself,
    // unaffected by polluting placeholder bounds in its subtree). Falls back
    // to the subtree union when the prim itself has no usable bounds (Xform
    // parents). Returns invalid when neither has anything.
    AABB anchorBounds(const USDScene& scene, PrimHandle prim) const;

    // World translation to anchor a gizmo on for `prim`. If `prim`'s own world
    // translation is the world origin (typical of intermediate Xforms below a
    // `!resetXformStack!` prim — Pixar's convention for asset-local "Geom"
    // groups), walks up the parent chain until it finds a non-zero translation.
    // Returns (0,0,0) when no ancestor has one either.
    glm::vec3 anchorPivot(const USDScene& scene, PrimHandle prim) const;

    const BoundsCache& bounds() const { return m_bounds; }

private:
    BoundsCache m_bounds;
    SpatialIndex m_spatial;
};
