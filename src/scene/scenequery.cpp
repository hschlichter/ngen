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

AABB SceneQuerySystem::anchorBounds(const USDScene& scene, PrimHandle prim) const {
    if (const auto* rec = m_bounds.get(prim); rec && rec->valid && rec->worldBounds.valid() && rec->worldBounds.min != rec->worldBounds.max) {
        return rec->worldBounds;
    }
    return subtreeBounds(scene, prim);
}

glm::vec3 SceneQuerySystem::anchorPivot(const USDScene& scene, PrimHandle prim) const {
    for (auto cur = prim; (bool) cur;) {
        const auto* xf = scene.getTransform(cur);
        if (!xf) {
            break;
        }
        auto pos = glm::vec3(xf->world[3]);
        if (pos != glm::vec3(0.0f)) {
            return pos;
        }
        const auto* rec = scene.getPrimRecord(cur);
        if (!rec || !rec->parent) {
            break;
        }
        cur = rec->parent;
    }
    return glm::vec3(0.0f);
}

AABB SceneQuerySystem::subtreeBounds(const USDScene& scene, PrimHandle prim) const {
    AABB result; // invalid by default
    if (!prim) {
        return result;
    }

    auto absorb = [&](const AABB& b) {
        if (!b.valid()) {
            return;
        }
        // Skip degenerate single-point bounds — these come from renderable prims
        // with no mesh data (binding->mesh null or empty vertices) whose default
        // localBounds collapsed to a point. They'd pollute the union and pull
        // the subtree's extent toward whatever world origin they sit at.
        if (b.min == b.max) {
            return;
        }
        if (!result.valid()) {
            result = b;
        } else {
            result.min = glm::min(result.min, b.min);
            result.max = glm::max(result.max, b.max);
        }
    };

    // DFS walk via firstChild/nextSibling. Iterative to avoid the std::function
    // allocation hot-path, since this is called from per-frame UI code.
    std::vector<PrimHandle> stack;
    stack.push_back(prim);
    while (!stack.empty()) {
        auto h = stack.back();
        stack.pop_back();
        if (const auto* rec = m_bounds.get(h); rec && rec->valid) {
            absorb(rec->worldBounds);
        }
        for (auto c = scene.firstChild(h); (bool) c; c = scene.nextSibling(c)) {
            stack.push_back(c);
        }
    }
    return result;
}
