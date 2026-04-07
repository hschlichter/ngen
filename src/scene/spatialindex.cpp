#include "spatialindex.h"

#include <algorithm>

static bool frustumContainsAABB(const Frustum& frustum, const AABB& aabb) {
    for (int i = 0; i < 6; i++) {
        auto& p = frustum.planes[i];
        // Test the positive vertex (the corner most aligned with the plane normal)
        glm::vec3 pv = {
            p.x > 0 ? aabb.max.x : aabb.min.x,
            p.y > 0 ? aabb.max.y : aabb.min.y,
            p.z > 0 ? aabb.max.z : aabb.min.z,
        };
        if (glm::dot(glm::vec3(p), pv) + p.w < 0) {
            return false;
        }
    }
    return true;
}

static bool rayIntersectsAABB(const Ray& ray, const AABB& aabb, float maxDist) {
    float tmin = 0.0f;
    float tmax = maxDist;

    for (int i = 0; i < 3; i++) {
        float invD = 1.0f / ray.direction[i];
        float t0 = (aabb.min[i] - ray.origin[i]) * invD;
        float t1 = (aabb.max[i] - ray.origin[i]) * invD;
        if (invD < 0.0f) {
            std::swap(t0, t1);
        }
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
        if (tmax < tmin) {
            return false;
        }
    }
    return true;
}

void SpatialIndex::rebuild(const BoundsCache& bounds, std::span<const PrimRuntimeRecord> prims) {
    m_nodes.clear();
    m_root = 0;

    // Collect prims with valid bounds
    std::vector<PrimHandle> leafPrims;
    for (const auto& prim : prims) {
        if (!(prim.flags & PrimFlagRenderable)) {
            continue;
        }
        const auto* rec = bounds.get(prim.handle);
        if (rec && rec->valid) {
            leafPrims.push_back(prim.handle);
        }
    }

    if (leafPrims.empty()) {
        return;
    }

    // Reserve space (2N-1 nodes for N leaves)
    m_nodes.reserve(leafPrims.size() * 2);
    m_root = buildRecursive(leafPrims, bounds, 0, (uint32_t) leafPrims.size());
}

uint32_t SpatialIndex::buildRecursive(std::vector<PrimHandle>& prims, const BoundsCache& bounds, uint32_t start, uint32_t end) {
    uint32_t count = end - start;

    // Leaf
    if (count == 1) {
        auto idx = (uint32_t) m_nodes.size();
        Node node = {};
        node.prim = prims[start];
        const auto* rec = bounds.get(node.prim);
        node.bounds = rec->worldBounds;
        m_nodes.push_back(node);
        return idx;
    }

    // Compute combined bounds
    AABB combined = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
    for (uint32_t i = start; i < end; i++) {
        const auto* rec = bounds.get(prims[i]);
        if (rec && rec->valid) {
            combined.min = glm::min(combined.min, rec->worldBounds.min);
            combined.max = glm::max(combined.max, rec->worldBounds.max);
        }
    }

    // Split along longest axis at midpoint
    auto extent = combined.max - combined.min;
    int axis = 0;
    if (extent.y > extent.x) {
        axis = 1;
    }
    if (extent.z > extent[axis]) {
        axis = 2;
    }
    float mid = (combined.min[axis] + combined.max[axis]) * 0.5f;

    auto* begin = prims.data() + start;
    auto* endPtr = prims.data() + end;
    auto* midPtr = std::partition(begin, endPtr, [&](PrimHandle h) {
        const auto* rec = bounds.get(h);
        if (!rec || !rec->valid) {
            return true;
        }
        float center = (rec->worldBounds.min[axis] + rec->worldBounds.max[axis]) * 0.5f;
        return center < mid;
    });

    auto splitIdx = (uint32_t) (midPtr - prims.data());
    // Avoid degenerate splits
    if (splitIdx == start || splitIdx == end) {
        splitIdx = start + count / 2;
    }

    // Allocate internal node
    auto idx = (uint32_t) m_nodes.size();
    m_nodes.push_back({}); // placeholder

    auto left = buildRecursive(prims, bounds, start, splitIdx);
    auto right = buildRecursive(prims, bounds, splitIdx, end);

    m_nodes[idx].bounds = combined;
    m_nodes[idx].left = left;
    m_nodes[idx].right = right;
    return idx;
}

void SpatialIndex::refit(const BoundsCache& bounds) {
    // Bottom-up refit: leaves first, then parents
    for (int i = (int) m_nodes.size() - 1; i >= 0; i--) {
        auto& node = m_nodes[i];
        if (node.isLeaf()) {
            const auto* rec = bounds.get(node.prim);
            if (rec && rec->valid) {
                node.bounds = rec->worldBounds;
            }
        } else {
            auto& l = m_nodes[node.left];
            auto& r = m_nodes[node.right];
            node.bounds.min = glm::min(l.bounds.min, r.bounds.min);
            node.bounds.max = glm::max(l.bounds.max, r.bounds.max);
        }
    }
}

void SpatialIndex::queryFrustum(const Frustum& frustum, std::vector<PrimHandle>& outPrims) const {
    if (m_nodes.empty()) {
        return;
    }

    // Stack-based traversal
    std::vector<uint32_t> stack;
    stack.push_back(m_root);

    while (!stack.empty()) {
        auto idx = stack.back();
        stack.pop_back();

        const auto& node = m_nodes[idx];
        if (!frustumContainsAABB(frustum, node.bounds)) {
            continue;
        }

        if (node.isLeaf()) {
            outPrims.push_back(node.prim);
        } else {
            stack.push_back(node.left);
            stack.push_back(node.right);
        }
    }
}

void SpatialIndex::queryRay(const Ray& ray, float maxDistance, std::vector<PrimHandle>& outPrims) const {
    if (m_nodes.empty()) {
        return;
    }

    std::vector<uint32_t> stack;
    stack.push_back(m_root);

    while (!stack.empty()) {
        auto idx = stack.back();
        stack.pop_back();

        const auto& node = m_nodes[idx];
        if (!rayIntersectsAABB(ray, node.bounds, maxDistance)) {
            continue;
        }

        if (node.isLeaf()) {
            outPrims.push_back(node.prim);
        } else {
            stack.push_back(node.left);
            stack.push_back(node.right);
        }
    }
}
