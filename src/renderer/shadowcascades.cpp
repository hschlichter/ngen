#include "shadowcascades.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

auto shadowAtlasExtent(const ShadowCascadeSettings& settings) -> RhiExtent2D {
    uint32_t tile = std::max(64u, settings.tileSize);
    if (settings.count <= 1) {
        return {tile, tile};
    }
    return {tile * 2, tile * 2};
}

auto pickShadowLightDirection(std::span<const RenderLight> lights, glm::vec3 worldUp) -> glm::vec3 {
    const RenderLight* shadowLight = nullptr;
    const RenderLight* fallback = nullptr;
    for (const auto& l : lights) {
        if (l.type != LightType::Directional) {
            continue;
        }
        if (fallback == nullptr) {
            fallback = &l;
        }
        if (l.shadowEnable && shadowLight == nullptr) {
            shadowLight = &l;
        }
    }
    const RenderLight* picked = shadowLight != nullptr ? shadowLight : fallback;
    if (picked == nullptr) {
        return worldUp;
    }
    auto raw = glm::vec3(picked->worldTransform[2]);
    return glm::dot(raw, raw) > 1e-6f ? glm::normalize(raw) : worldUp;
}

auto sceneBoundsOf(std::span<const RenderMeshInstance> instances) -> AABB {
    AABB bounds = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
    for (const auto& inst : instances) {
        if (inst.worldBounds.valid()) {
            bounds.min = glm::min(bounds.min, inst.worldBounds.min);
            bounds.max = glm::max(bounds.max, inst.worldBounds.max);
        }
    }
    return bounds;
}

namespace {

// Practical split scheme: a blend of logarithmic and uniform distances.
auto splitDistance(float nearZ, float farZ, float fraction, float lambda) -> float {
    float uniform = nearZ + ((farZ - nearZ) * fraction);
    float logarithmic = nearZ * std::pow(farZ / nearZ, fraction);
    return (lambda * logarithmic) + ((1.0f - lambda) * uniform);
}

} // namespace

auto fitShadowCascades(const ShadowCascadeSettings& settings, const glm::mat4& view, const glm::mat4& proj, float nearZ, float farZ, glm::vec3 lightDir, glm::vec3 worldUp, const AABB& sceneBounds, std::span<ShadowCascade> out) -> uint32_t {
    uint32_t count = std::clamp(settings.count, 1u, std::min(maxShadowCascades, (uint32_t) out.size()));
    uint32_t tile = std::max(64u, settings.tileSize);
    auto invView = glm::inverse(view);
    auto cameraPos = glm::vec3(invView[3]);

    // Far plane pulled in to the scene: the distance from the camera to the farthest bounds
    // corner. Cascades past the last mesh would shadow nothing.
    if (sceneBounds.valid()) {
        float farthest = 0.0f;
        for (int i = 0; i < 8; i++) {
            glm::vec3 corner = {
                (i & 1) != 0 ? sceneBounds.max.x : sceneBounds.min.x,
                (i & 2) != 0 ? sceneBounds.max.y : sceneBounds.min.y,
                (i & 4) != 0 ? sceneBounds.max.z : sceneBounds.min.z,
            };
            farthest = std::max(farthest, glm::length(corner - cameraPos));
        }
        farZ = std::clamp(farthest, nearZ * 4.0f, farZ);
    }

    // View-space half extents per unit depth from the projection; the Vulkan y flip only
    // changes the sign, which the absolute value removes.
    float tanHalfX = 1.0f / std::abs(proj[0][0]);
    float tanHalfY = 1.0f / std::abs(proj[1][1]);

    // A light "up" that is never parallel to the light direction.
    auto lightUp = std::abs(glm::dot(lightDir, worldUp)) > 0.99f ? glm::normalize(glm::cross(lightDir, glm::vec3(1.0f, 0.0f, 0.0f))) : worldUp;

    float sliceNear = nearZ;
    for (uint32_t c = 0; c < count; c++) {
        float sliceFar = c + 1 == count ? farZ : splitDistance(nearZ, farZ, (float) (c + 1) / (float) count, settings.splitLambda);

        // Slice corners in world space, then the bounding sphere: its radius depends on the
        // slice alone, so the frustum size is stable while the camera turns.
        std::array<glm::vec3, 8> corners;
        int i = 0;
        for (float depth : {sliceNear, sliceFar}) {
            for (auto [sx, sy] : {std::pair{-1.0f, -1.0f}, std::pair{1.0f, -1.0f}, std::pair{1.0f, 1.0f}, std::pair{-1.0f, 1.0f}}) {
                glm::vec4 viewPos = {sx * depth * tanHalfX, sy * depth * tanHalfY, -depth, 1.0f};
                corners[i++] = glm::vec3(invView * viewPos);
            }
        }
        glm::vec3 center(0.0f);
        for (const auto& p : corners) {
            center += p;
        }
        center /= 8.0f;
        float radius = 0.0f;
        for (const auto& p : corners) {
            radius = std::max(radius, glm::length(p - center));
        }
        radius = std::ceil(radius * 16.0f) / 16.0f; // quantised so it does not drift frame to frame

        // Light view from far enough along the light direction that the scene bounds fit in
        // front of the near plane: casters between the light and the slice must still cast.
        float reach = radius;
        if (sceneBounds.valid()) {
            reach = std::max(reach, glm::length(sceneBounds.max - sceneBounds.min));
        }
        auto lightPos = center + (lightDir * (reach + radius));
        auto lightView = glm::lookAt(lightPos, center, lightUp);

        float zNear = 0.01f;
        float zFar = reach + (radius * 2.0f) + 0.01f;
        if (sceneBounds.valid()) {
            auto lightSpace = sceneBounds.transformed(lightView);
            zNear = std::max(0.01f, std::min(zNear, -lightSpace.max.z));
        }
        auto lightProj = glm::ortho(-radius, radius, -radius, radius, zNear, zFar);

        // Snap the projection's origin to whole texels, so shadows do not shimmer when the
        // camera moves by fractions of a texel.
        auto viewProj = lightProj * lightView;
        glm::vec4 origin = viewProj * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        origin *= (float) tile / 2.0f;
        glm::vec2 rounded = glm::round(glm::vec2(origin));
        glm::vec2 offset = (rounded - glm::vec2(origin)) * (2.0f / (float) tile);
        lightProj[3][0] += offset.x;
        lightProj[3][1] += offset.y;
        viewProj = lightProj * lightView;

        auto& cascade = out[c];
        cascade.viewProj = viewProj;
        cascade.splitFar = sliceFar;
        cascade.texelWorldSize = (radius * 2.0f) / (float) tile;
        cascade.texelDepthNdc = cascade.texelWorldSize / (zFar - zNear);
        if (count == 1) {
            cascade.atlasRect = {0.0f, 0.0f, 1.0f, 1.0f};
        } else {
            cascade.atlasRect = {(float) (c % 2) * 0.5f, (float) (c / 2) * 0.5f, 0.5f, 0.5f};
        }
        sliceNear = sliceFar;
    }
    return count;
}
