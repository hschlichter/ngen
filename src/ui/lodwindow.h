#pragma once

#include "lodtool.h"

#include <glm/glm.hpp>

#include <array>
#include <string>
#include <vector>

// The LOD window's own state: what it scrolled to, the "Set shown" choice, and the inspector's edit buffers. What it lists and inspects is
// the LOD tool's (LodToolState, src/tools/lodtool.h).
struct LodWindowState {
    std::string scrolledTo; // the selected root the list last scrolled to
    int allChoice = 0;      // the "Set shown" choice: 0 default, 1 coarsest, 2 + n level n
    // Edit buffers for the inspector's threshold fields, by heuristic index; reset when the inspected root changes.
    std::string editedRoot;
    std::vector<std::array<char, 256>> thresholdText;
    std::vector<std::array<char, 256>> blendText;
};

// The LOD tool: every LOD root placed, both kinds, in one list (filtered by path, kind, under the selection, not drawing what it wants,
// fading; click selects, double-click frames), and below it the inspector of the selected root: its heuristics (each one's thresholds in
// force, editable, with the authored ones beside; its measure now and the level it would pick), its levels (triangles, draws), and its
// state. The header holds the global controls (heuristic, preference, bias, hysteresis, blending, unload delay) and the viewport's layers
// (the LOD Level view with its legend, labels, the gizmo, the pop flash, the Triangle Size view).
auto drawLodWindow(bool& show, LodToolState& state, LodWindowState& window) -> void;
// The labels layer: over each root the list shows, inside the viewport, its name, drawn level (→ the level fading in and its share) and
// measure; a click selects it. `viewProj` maps world metres to clip space; `viewport` is in ImGui's display units. Inside the ImGui frame.
auto drawLodLabels(LodToolState& state, const glm::mat4& viewProj, glm::vec2 viewport) -> void;
