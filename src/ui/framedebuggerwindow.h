#pragma once

#include "capture.h"
#include "framedebug.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Persistent state of the Frame Debugger window.
struct FrameDebuggerState {
    std::optional<FrameDebugCapture> capture;
    int selectedPass = -1;
    bool requestCapture = false; // read by main: record the next frame
    bool watchResources = false; // Resources tab visible: capture the selected pass's writes
    bool watchDraws = false;     // Draws tab visible: capture the draw commands and counts
    uint32_t trigger = 0;
    std::map<uint32_t, CaptureResult> results; // capture watch id -> latest result
    char commandFilter[64] = "";
};

inline constexpr uint32_t frameDebuggerWatchBase = 1000;
inline constexpr uint32_t frameDebuggerCommandsWatch = 900;
inline constexpr uint32_t frameDebuggerCountsWatch = 901;

// Pass by pass through one recorded frame: accesses, barriers with their backend details,
// the command log, bound descriptor sets and their contents, the resources a pass writes
// (before and after, through capture watches) and, for indirect passes, the draw commands.
// primPathOfInstance resolves an instance index to its prim path.
auto drawFrameDebuggerWindow(bool& show, FrameDebuggerState& state, const std::function<std::string(uint32_t)>& primPathOfInstance, std::function<void(const std::string&, const std::string&)> openInCapture) -> void;
// Capture watches the window wants this frame.
auto frameDebuggerWatches(const FrameDebuggerState& state) -> std::vector<CaptureWatch>;
