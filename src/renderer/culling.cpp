#include "culling.h"

#include "profile.h"

#include <algorithm>

auto CullState::update(const glm::mat4& liveViewProj) -> glm::mat4 {
    if (frozen) {
        if (!frozenActive) {
            frozenViewProj = liveViewProj;
            frozenActive = true;
        }
        return frozenViewProj;
    }
    frozenActive = false;
    return liveViewProj;
}

auto CullState::frozenCorners() const -> std::array<glm::vec3, 8> {
    return Frustum::corners(frozenViewProj);
}

auto cullShadowCascades(std::span<const ShadowCascade> cascades, std::span<const RenderMeshInstance> instances, ShadowCullResult& out) -> void {
    PROFILE_ZONE("CullShadow");
    out.count = (uint32_t) std::min(cascades.size(), (size_t) maxShadowCascades);
    uint32_t totalCulled = 0;
    for (uint32_t c = 0; c < maxShadowCascades; c++) {
        out.visible[c].clear();
        out.culled[c] = 0;
        out.drawn[c] = 0;
        if (c >= out.count) {
            continue;
        }
        auto frustum = Frustum::fromViewProj(cascades[c].viewProj);
        out.visible[c].resize(instances.size(), 1);
        for (size_t i = 0; i < instances.size(); i++) {
            const auto& inst = instances[i];
            bool inside = !inst.worldBounds.valid() || frustum.contains(inst.worldBounds);
            out.visible[c][i] = inside ? 1 : 0;
            if (inst.primFirst) {
                if (inside) {
                    out.drawn[c]++;
                } else {
                    out.culled[c]++;
                }
            }
        }
        totalCulled += out.culled[c];
    }
    PROFILE_ZONE_VALUE(totalCulled);
}

auto cullInstances(const CullState& state, const glm::mat4& viewProj, std::span<const RenderMeshInstance> instances, std::vector<uint8_t>& visible) -> uint32_t {
    PROFILE_ZONE("Cull");
    visible.clear();
    uint32_t culled = 0;
    if (state.enabled) {
        auto frustum = Frustum::fromViewProj(viewProj);
        visible.resize(instances.size(), 1);
        for (size_t i = 0; i < instances.size(); i++) {
            const auto& bounds = instances[i].worldBounds;
            bool inside = !bounds.valid() || frustum.contains(bounds);
            visible[i] = inside ? 1 : 0;
            if (!inside) {
                culled++;
            }
        }
    }
    PROFILE_ZONE_VALUE(culled);
    return culled;
}
