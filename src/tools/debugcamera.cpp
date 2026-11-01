#include "debugcamera.h"

#include "debugdraw.h"
#include "scenetypes.h"

auto placeDebugCamera(DebugCameraState& debug, const Camera& main, double metersPerUnit) -> void {
    auto unitsPerMetre = (float) (1.0 / metersPerUnit);
    // Behind along the main camera's heading across the ground, so a camera looking straight down still gets an offset.
    auto heading = main.forward() - (main.worldUp * glm::dot(main.forward(), main.worldUp));
    if (glm::dot(heading, heading) < 1e-8f) {
        heading = main.right();
    }
    heading = glm::normalize(heading);
    debug.camera = main;
    debug.camera.position = main.position - (heading * (15.0f * unitsPerMetre)) + (main.worldUp * (10.0f * unitsPerMetre));
    debug.camera.lookAlong(main.position - debug.camera.position);
}

auto extendDebugCameraTrail(DebugCameraState& debug, const glm::vec3& mainPosition, double metersPerUnit) -> void {
    auto spacing = (float) (DebugCameraState::trailSpacingMetres / metersPerUnit);
    if (!debug.trailPoints.empty() && glm::length(mainPosition - debug.trailPoints.back()) < spacing) {
        return;
    }
    debug.trailPoints.push_back(mainPosition);
    if (debug.trailPoints.size() > DebugCameraState::trailCount) {
        debug.trailPoints.erase(debug.trailPoints.begin());
    }
}

auto lodCamera(const DebugCameraState& debug, const Camera& main) -> const Camera& {
    return toolsconfig::debugCamera.get() && toolsconfig::debugLod.get() ? debug.camera : main;
}

auto drawMainCamera(const DebugCameraState& debug, const Camera& main, const glm::mat4& mainViewProj, float farDepth, double metersPerUnit, DebugDraw& draw) -> void {
    const glm::vec4 frustumColor = {1.0f, 0.55f, 0.1f, 1.0f};
    const glm::vec4 trailColor = {1.0f, 0.55f, 0.1f, 0.6f};
    // The frustum from its near plane to farDepth: the side edges and both rectangles.
    auto c = Frustum::corners(mainViewProj, 1.0f, farDepth);
    for (int i = 0; i < 4; i++) {
        int next = (i + 1) % 4;
        draw.line(c[i], c[next], frustumColor);
        draw.line(c[4 + i], c[4 + next], frustumColor);
        draw.line(c[i], c[4 + i], frustumColor);
    }
    // The body: a box half a metre across at the eye, and its forward axis two metres long.
    auto unitsPerMetre = (float) (1.0 / metersPerUnit);
    auto half = glm::vec3(0.25f * unitsPerMetre);
    draw.box({.min = main.position - half, .max = main.position + half}, frustumColor);
    draw.line(main.position, main.position + (main.forward() * (2.0f * unitsPerMetre)), frustumColor);
    if (toolsconfig::debugTrail.get()) {
        for (size_t i = 1; i < debug.trailPoints.size(); i++) {
            draw.line(debug.trailPoints[i - 1], debug.trailPoints[i], trailColor);
        }
        if (!debug.trailPoints.empty()) {
            draw.line(debug.trailPoints.back(), main.position, trailColor);
        }
    }
}

namespace toolsconfig {

config::Bool debugCamera{"tools.debugcamera.on", false, "Render from a second camera; the main camera (the player) keeps driving what is decided from a viewpoint (Ctrl+D)."};
config::Bool debugControlMain{"tools.debugcamera.controlMain", false, "The keys and mouse move the main camera while the view stays on the debug camera (C)."};
config::Bool debugLod{"tools.debugcamera.lod", false, "Measure the LOD heuristic, residency and render LOD from the debug camera instead of the player."};
config::Bool debugCull{"tools.debugcamera.cull", false, "Cull from the debug camera: draw what it sees instead of what the player sees."};
config::Bool debugShadows{"tools.debugcamera.shadows", false, "Fit the shadow cascades to the debug camera's view instead of the player's."};
config::Bool debugTrail{"tools.debugcamera.trail", true, "Draw the main camera's trail, a point every 2 m it moved."};

} // namespace toolsconfig
