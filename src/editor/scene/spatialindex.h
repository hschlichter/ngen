#pragma once

#include "boundscache.h"
#include "scenehandles.h"
#include "scenetypes.h"
#include "usdscene.h"

#include <span>
#include <vector>

class SpatialIndex {
public:
    void rebuild(const BoundsCache& bounds, std::span<const PrimRuntimeRecord> prims);
    void refit(const BoundsCache& bounds);

    void queryFrustum(const Frustum& frustum, std::vector<PrimHandle>& outPrims) const;
    void queryRay(const Ray& ray, float maxDistance, std::vector<PrimHandle>& outPrims) const;

private:
    struct Node {
        AABB bounds;
        uint32_t left = 0;
        uint32_t right = 0;
        PrimHandle prim;
        bool isLeaf() const { return prim.index != 0; }
    };

    std::vector<Node> m_nodes;
    uint32_t m_root = 0;

    uint32_t buildRecursive(std::vector<PrimHandle>& prims, const BoundsCache& bounds, uint32_t start, uint32_t end);
};
