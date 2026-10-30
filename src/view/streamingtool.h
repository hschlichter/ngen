#pragma once

#include "streamingwindow.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>

class DebugDraw;
class ViewScene;

// The Streaming tool's data (src/view/README.md, "The Streaming tool"): what the Streaming window lists, inspects, maps and draws in the
// viewport, built from the scene's snapshot: the streaming roots placed, their residency and what streaming measured.

// The rows, from a snapshot: every streaming root drawn, its state for the colours, its radii in force and its bounds in world metres; the
// totals; and per streaming root slot its state for the streaming view.
auto buildStreamingRows(const ViewScene& scene, StreamingWindowState& state) -> void;
// One root inspected: the streaming root at `path`, its heuristics by domain measured from `eye` (world metres; a screen size over the view
// window `windowArea`); none when no streaming root is placed there.
auto inspectStreamingRoot(const ViewScene& scene, const std::string& path, const glm::vec3& eye, float windowArea) -> std::optional<StreamingInspection>;
// The viewport's layers: each shown root's bounds in its state's colour, and the load (solid) and unload (faint) radii of the selected root,
// or of every shown root, per boundary around its bounds, in the plane across `up` through the bounds' middle.
auto drawStreamingOverlays(const StreamingWindowState& state, DebugDraw& draw, const glm::vec3& up) -> void;
