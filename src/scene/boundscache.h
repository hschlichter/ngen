#pragma once

#include "scenehandles.h"
#include "scenetypes.h"

#include <cstdint>
#include <span>
#include <vector>

class USDScene;
class MeshLibrary;

struct BoundsCacheRecord {
    AABB localBounds;
    AABB worldBounds;
    bool valid = false;
    uint32_t lastFrame = 0;
};

class BoundsCache {
public:
    void rebuild(const USDScene& scene, const MeshLibrary& meshLib);
    void updateDirty(const USDScene& scene, const MeshLibrary& meshLib, std::span<const PrimHandle> dirtyPrims, uint32_t frame);

    const BoundsCacheRecord* get(PrimHandle h) const;

private:
    std::vector<BoundsCacheRecord> m_records;
};
