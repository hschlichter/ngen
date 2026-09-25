#pragma once

#include "capture.h"
#include "debugview.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

inline constexpr uint32_t debugViewReadoutWatch = 3000;

// The Debug View window: the active view's colour scale and the raw value under the cursor.
struct DebugViewWindowState {
    std::optional<CaptureResult> readout; // one texel of debugview.value
    uint32_t cursorX = 0;                 // framebuffer pixels
    uint32_t cursorY = 0;
    bool cursorValid = false; // over the viewport, not over a window
};

// Reads the mouse position for the next readout. Call once per frame before drawing.
auto updateDebugViewCursor(DebugViewWindowState& state) -> void;
// The live one-texel watch under the cursor, when a view is on and the cursor is valid.
auto debugViewReadoutWatchFor(const DebugViewWindowState& state, DebugView view) -> std::optional<CaptureWatch>;
// instancePath names an instance index (its prim path).
auto drawDebugViewWindow(DebugView view, DebugViewWindowState& state, const std::function<std::string(uint32_t)>& instancePath) -> void;
