#include "culling.h"

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
