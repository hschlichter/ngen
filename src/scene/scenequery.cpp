#include "scenequery.h"
#include "mesh.h"
#include "usdscene.h"

void SceneQuerySystem::rebuild(const USDScene& scene, const MeshLibrary& meshLib) {
    m_bounds.rebuild(scene, meshLib);
    m_spatial.rebuild(m_bounds, scene.allPrims());
}

void SceneQuerySystem::updateDirty(const USDScene& scene, const MeshLibrary& meshLib, std::span<const PrimHandle> dirtyPrims, uint32_t frame) {
    m_bounds.updateDirty(scene, meshLib, dirtyPrims, frame);
    m_spatial.refit(m_bounds);
}

bool SceneQuerySystem::raycast(const Ray& ray, float maxDistance, RaycastHit& outHit) const {
    std::vector<PrimHandle> candidates;
    m_spatial.queryRay(ray, maxDistance, candidates);

    bool hit = false;
    float closest = maxDistance;

    for (auto h : candidates) {
        const auto* rec = m_bounds.get(h);
        if (!rec || !rec->valid) {
            continue;
        }

        // Ray-AABB intersection for distance
        float tmin = 0.0f;
        float tmax = closest;
        bool intersects = true;

        for (int i = 0; i < 3; i++) {
            float invD = 1.0f / ray.direction[i];
            float t0 = (rec->worldBounds.min[i] - ray.origin[i]) * invD;
            float t1 = (rec->worldBounds.max[i] - ray.origin[i]) * invD;
            if (invD < 0.0f) {
                std::swap(t0, t1);
            }
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
            if (tmax < tmin) {
                intersects = false;
                break;
            }
        }

        if (intersects && tmin < closest) {
            closest = tmin;
            outHit.prim = h;
            outHit.distance = tmin;
            outHit.position = ray.origin + ray.direction * tmin;
            outHit.normal = glm::vec3(0, 1, 0); // approximate
            hit = true;
        }
    }

    return hit;
}

void SceneQuerySystem::frustumCull(const Frustum& frustum, std::vector<PrimHandle>& outPrims) const {
    m_spatial.queryFrustum(frustum, outPrims);
}
