#include "culling.h"

#include "renderconfig.h"

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

auto CullState::frozenCorners(float farDepth) const -> std::array<glm::vec3, 8> {
    return Frustum::corners(frozenViewProj, 1.0f, farDepth);
}

namespace renderconfig {

config::Bool cullingEnabled{"render.culling.enabled", true, "GPU culling of the instances against the camera's and the cascades' frustums; off draws every instance."};
config::Bool cullingFrozen{"render.culling.frozen", false, "Cull against the frustum as it was when frozen, so the culled set can be looked at from elsewhere."};
config::Bool cullingOcclusion{"render.culling.occlusion",
                              true,
                              "Two-phase occlusion culling: the camera's instances and meshlets tested against a depth pyramid of what was visible last frame; runs the "
                              "depth prepass. While the culling camera isn't the rendered one (a debug camera, a frozen frustum), from the culling camera into a "
                              "depth of its own. Skipped while culling is off."};

} // namespace renderconfig
