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

    const BoundsCache& bounds() const { return m_bounds; }

private:
    BoundsCache m_bounds;
    SpatialIndex m_spatial;
};
