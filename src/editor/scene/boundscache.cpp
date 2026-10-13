#include "boundscache.h"
#include "mesh.h"
#include "usdscene.h"

static AABB computeLocalBounds(const MeshDesc* meshData) {
    if (!meshData || meshData->vertices.empty()) {
        return {};
    }

    AABB bounds = {
        .min = glm::vec3(1e30f),
        .max = glm::vec3(-1e30f),
    };
    for (const auto& v : meshData->vertices) {
        auto p = glm::vec3(v.position[0], v.position[1], v.position[2]);
        bounds.min = glm::min(bounds.min, p);
        bounds.max = glm::max(bounds.max, p);
    }
    return bounds;
}

void BoundsCache::rebuild(const USDScene& scene, const MeshLibrary& meshLib) {
    auto prims = scene.allPrims();
    // +1 for the null index 0 slot
    m_records.clear();
    m_records.resize(prims.size() + 1);

    for (const auto& prim : prims) {
        if (!(prim.flags & PrimFlagRenderable)) {
            continue;
        }

        auto& rec = m_records[prim.handle.index];
        const auto* binding = scene.getAssetBinding(prim.handle);
        if (binding && binding->mesh) {
            rec.localBounds = computeLocalBounds(meshLib.get(binding->mesh));
        }

        const auto* xf = scene.getTransform(prim.handle);
        if (xf) {
            rec.worldBounds = rec.localBounds.transformed(xf->world);
        } else {
            rec.worldBounds = rec.localBounds;
        }
        rec.valid = true;
        rec.lastFrame = scene.frameIndex();
    }
}

void BoundsCache::updateDirty(const USDScene& scene, const MeshLibrary& meshLib, std::span<const PrimHandle> dirtyPrims, uint32_t frame) {
    for (auto h : dirtyPrims) {
        if (h.index == 0 || h.index >= m_records.size()) {
            continue;
        }

        auto& rec = m_records[h.index];
        const auto* binding = scene.getAssetBinding(h);
        if (binding && binding->mesh) {
            rec.localBounds = computeLocalBounds(meshLib.get(binding->mesh));
        }

        const auto* xf = scene.getTransform(h);
        if (xf) {
            rec.worldBounds = rec.localBounds.transformed(xf->world);
        } else {
            rec.worldBounds = rec.localBounds;
        }
        rec.valid = true;
        rec.lastFrame = frame;
    }
}

const BoundsCacheRecord* BoundsCache::get(PrimHandle h) const {
    if (h.index == 0 || h.index >= m_records.size()) {
        return nullptr;
    }
    return &m_records[h.index];
}
