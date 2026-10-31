#pragma once

#include "streamingtool.h"

#include <glm/glm.hpp>

#include <array>
#include <string>

// The Streaming window's own state: what it scrolled to, the map's pan and zoom, and the inspector's edit buffers. What it lists, inspects
// and maps is the Streaming tool's (StreamingToolState, src/tools/streamingtool.h).
struct StreamingWindowState {
    std::string scrolledTo;
    // The map: where it looks (world metres, in the plane across the camera's up), metres per pixel, and whether it follows the camera.
    glm::vec2 mapCenter = glm::vec2(0.0f);
    float mapMetresPerPixel = 4.0f;
    bool mapFollow = true;
    // Edit buffers for the inspector's radii, reset when the inspected root changes.
    std::string editedRoot;
    std::array<char, 256> loadText = {};
    std::array<char, 256> unloadText = {};
};

// The Streaming window: an overview (counters, the strip chart), the roots (filtered; click selects, double-click frames), the inspector of
// the selected root (its heuristics by domain, radii authored and in force, editable; its levels; its state and why; findings), the
// measurement controls (reset, fly a camera path), the viewport's layers, and the map of the roots around the camera.
auto drawStreamingWindow(bool& show, StreamingToolState& state, StreamingWindowState& window) -> void;
// The labels layer: over each root the list shows, its name, drawn / selected level, state and its state's name; a click selects it.
auto drawStreamingLabels(StreamingToolState& state, const glm::mat4& viewProj, glm::vec2 viewport) -> void;
