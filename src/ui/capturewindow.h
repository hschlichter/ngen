#pragma once

#include "capture.h"
#include "framegraphdebug.h"

#include <optional>
#include <string>

// Persistent state of the Capture window: the target (resource after a pass), how to show
// it, and the last result.
struct CaptureWindowState {
    std::string pass; // "" = after the last pass
    std::string resource;
    bool live = false;
    uint32_t trigger = 0;
    CaptureDisplay display;
    std::optional<CaptureResult> result;
    float zoom = 1.0f;
    int page = 0;
    bool hex = false;
    char dumpPath[256] = "capture";
    std::string status;
};

inline constexpr uint32_t captureWindowWatchId = 1;

// Capture any frame-graph resource after any pass, or a static scene buffer after the last
// pass; shows textures with channel and range controls and buffers as typed rows.
auto drawCaptureWindow(bool& show, const std::optional<FrameGraphDebugSnapshot>& fg, CaptureWindowState& state) -> void;
// The window's watch, when it has a target.
auto captureWindowWatch(const CaptureWindowState& state) -> std::optional<CaptureWatch>;
