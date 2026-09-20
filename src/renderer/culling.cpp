#include "culling.h"

#include "profile.h"

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
