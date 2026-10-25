#pragma once

#include "lodwindow.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>

class DebugDraw;
class ViewScene;
struct CullResult;

// The LOD tool's data (src/view/README.md, "The LOD tool"): what the LOD window lists, inspects and draws in the viewport, built from the
// scene's snapshot and the render-LOD readback.

// The camera the inspector measures from: its position in world metres, vertical field of view in radians, and aspect.
struct LodToolCamera {
    glm::vec3 eye = glm::vec3(0.0f);
    float fovY = 0.785f;
    float aspect = 1.0f;
};

// The list's rows, from a snapshot: every streaming root drawn and every render-LOD group placed, with their heuristics as the view picks
// with them (tuned) and their shapes for the viewport. The render-LOD rows' live state is updateLodRows'.
auto buildLodRows(const ViewScene& scene, LodWindowState& state) -> void;
// The render-LOD rows' live state from the latest readback with states: wanted, drawn, fading, measure, the pop flash.
auto updateLodRows(const ViewScene& scene, const CullResult& cull, LodWindowState& state) -> void;
// One root inspected: the LOD root at `path` (a streaming root's or a render-LOD group's full path); none when no root is placed there.
auto inspectLodRoot(const ViewScene& scene, const CullResult& cull, const std::string& path, const LodToolCamera& camera) -> std::optional<LodInspection>;
// The heuristic gizmo: for the selected root, or every root the list shows, its distance's center or box, or its screen size's sphere,
// and a ring (in the plane across `up`) at each threshold, at the distance where it is reached (a screen size's from the camera's window,
// `windowArea`), in the colour of the level beyond it; a fainter ring where each blend band ends.
auto drawLodGizmos(const LodWindowState& state, DebugDraw& draw, const glm::vec3& up, float windowArea) -> void;
// The distance unit a stage's metres per unit names: "m", "cm", else "units".
auto lodUnitName(double metersPerUnit) -> const char*;
