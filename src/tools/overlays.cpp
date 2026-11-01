#include "overlays.h"

#include "debugdraw.h"
#include "renderworld.h"

auto drawOverlays(DebugDraw& draw,
                  const RenderWorld& world,
                  const glm::vec3& eye,
                  const glm::vec3& up,
                  std::span<const uint8_t> visible,
                  const std::optional<std::array<glm::vec3, 8>>& frozenFrustum,
                  std::span<const ShadowCascade> cascades,
                  const AABB* selectedBounds) -> void {
    if (toolsconfig::overlayGrid.get()) {
        draw.grid(eye, up, 1.0f, 50, {0.25f, 0.25f, 0.25f, 1.0f});
    }
    if (toolsconfig::overlayOrigin.get()) {
        draw.sphere({0, 0, 0}, 0.1f, {1.0f, 0.9f, 0.2f, 1.0f}, 16);
    }
    if (toolsconfig::overlayAabbs.get()) {
        for (const auto& inst : world.meshInstances) {
            if (inst.alive && inst.worldBounds.valid()) {
                draw.box(inst.worldBounds, {0.0f, 1.0f, 0.0f, 1.0f});
            }
        }
    }
    // Culling debug: every instance AABB by its cull result, red for culled, green for drawn, and the frozen frustum's twelve edges in
    // yellow when the frustum is frozen.
    if (toolsconfig::overlayCulled.get() && visible.size() == world.meshInstances.size()) {
        for (size_t i = 0; i < visible.size(); i++) {
            const auto& inst = world.meshInstances[i];
            if (!inst.alive || !inst.worldBounds.valid()) {
                continue;
            }
            glm::vec4 color = visible[i] != 0 ? glm::vec4{0.2f, 1.0f, 0.2f, 1.0f} : glm::vec4{1.0f, 0.2f, 0.2f, 1.0f};
            draw.box(inst.worldBounds, color);
        }
    }
    if (frozenFrustum.has_value()) {
        const auto& c = *frozenFrustum;
        glm::vec4 yellow = {1.0f, 0.9f, 0.2f, 1.0f};
        for (int i = 0; i < 4; i++) {
            int next = (i + 1) % 4;
            draw.line(c[i], c[next], yellow);
            draw.line(c[4 + i], c[4 + next], yellow);
            draw.line(c[i], c[4 + i], yellow);
        }
    }
    // Each shadow cascade's light frustum (an ortho box) in its own colour: red, green, blue, yellow from near to far, the same colours
    // as the cascades view.
    if (toolsconfig::overlayCascadeFrusta.get()) {
        const std::array<glm::vec4, 4> colors = {{{1.0f, 0.3f, 0.3f, 1.0f}, {0.3f, 1.0f, 0.3f, 1.0f}, {0.3f, 0.5f, 1.0f, 1.0f}, {1.0f, 0.9f, 0.2f, 1.0f}}};
        for (size_t i = 0; i < cascades.size() && i < colors.size(); i++) {
            auto c = Frustum::corners(cascades[i].viewProj);
            for (int e = 0; e < 4; e++) {
                int next = (e + 1) % 4;
                draw.line(c[e], c[next], colors[i]);
                draw.line(c[4 + e], c[4 + next], colors[i]);
                draw.line(c[e], c[4 + e], colors[i]);
            }
        }
    }
    if (toolsconfig::overlayLightGizmos.get() && !world.lights.empty()) {
        // The sun floats a fixed distance in front of the camera along the toward-light direction: constant world size, so it reads the
        // same at any scene scale and never meets scene geometry. A directional compass, not an object in the scene.
        constexpr float sunDistance = 3.0f;
        constexpr float discRadius = 0.25f;
        constexpr float shaftLength = 0.75f;
        for (const auto& l : world.lights) {
            if (l.type != LightType::Directional) {
                continue;
            }
            // worldTransform[2] is the light's +Z axis: toward the light, by USD's convention.
            auto toward = glm::vec3(l.worldTransform[2]);
            if (glm::dot(toward, toward) < 1e-6f) {
                continue;
            }
            auto towardUnit = glm::normalize(toward);
            auto sunPos = eye + towardUnit * sunDistance;
            draw.sunLight(sunPos, -towardUnit, discRadius, shaftLength, {1.0f, 0.85f, 0.2f, 1.0f});
        }
    }
    if (selectedBounds != nullptr && toolsconfig::overlaySelectedAabb.get() && selectedBounds->valid()) {
        draw.box(*selectedBounds, {1.0f, 0.0f, 0.6f, 1.0f});
    }
}

namespace toolsconfig {

config::Bool overlayGrid{"tools.overlay.grid", true, "A ground grid on the scene's up axis."};
config::Bool overlayOrigin{"tools.overlay.origin", true, "The world's axes at the origin."};
config::Bool overlayGizmos{"tools.overlay.gizmos", true, "The corner axis gizmo and the selection's transform gizmo."};
config::Bool overlayAabbs{"tools.overlay.aabbs", false, "Every instance's world bounds."};
config::Bool overlaySelectedAabb{"tools.overlay.selectedAabb", true, "The selection's world bounds."};
config::Bool overlayLightGizmos{"tools.overlay.lightGizmos", true, "Each light's direction."};
config::Bool overlayCulled{"tools.overlay.culled", false, "Every instance's bounds by its GPU cull result: green drawn, red culled."};
config::Int overlayCulledView{"tools.overlay.culledView", 0, "Whose cull result the culled overlay shows: 0 the camera, 1 and up a shadow cascade.", {.min = 0, .max = 4}};
config::Bool overlayCascadeFrusta{"tools.overlay.cascadeFrusta", false, "Each shadow cascade's light frustum, in the cascades view's colours."};

} // namespace toolsconfig
