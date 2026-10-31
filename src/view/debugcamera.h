#pragma once

#include "camerawindow.h"
#include "config.h"

#include <glm/glm.hpp>

class DebugDraw;

// The debug camera (src/view/README.md, "The debug camera"): placed beside the main camera, the main camera's trail, and the main camera
// drawn as the debug camera sees it.

// Puts the debug camera 15 m behind and 10 m above the main camera, looking at it, with the main camera's speed and mouse sensitivity.
auto placeDebugCamera(DebugCameraState& debug, const Camera& main, double metersPerUnit) -> void;
// Adds the main camera's position to the trail once it is DebugCameraState::trailSpacingMetres from the last point; keeps the last
// DebugCameraState::trailCount.
auto extendDebugCameraTrail(DebugCameraState& debug, const glm::vec3& mainPosition, double metersPerUnit) -> void;
// The main camera: its frustum out to `farDepth` (`mainViewProj` is its view-projection), a box at its eye with a line along its forward
// axis, and its trail.
auto drawMainCamera(const DebugCameraState& debug, const Camera& main, const glm::mat4& mainViewProj, float farDepth, double metersPerUnit, DebugDraw& draw) -> void;

// The debug camera's configuration values (src/config/README.md).
namespace toolsconfig {
extern config::Bool debugCamera;      // tools.debugcamera.on: render from the debug camera
extern config::Bool debugControlMain; // tools.debugcamera.controlMain: the keys and mouse move the main camera
extern config::Bool debugLod;         // tools.debugcamera.lod: LOD and streaming measured from the debug camera
extern config::Bool debugCull;        // tools.debugcamera.cull: culling from the debug camera
extern config::Bool debugShadows;     // tools.debugcamera.shadows: the shadow cascades fitted to the debug camera
extern config::Bool debugTrail;       // tools.debugcamera.trail: the main camera's trail drawn
} // namespace toolsconfig
