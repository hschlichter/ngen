#pragma once

#include "capture.h"
#include "gpuscenecapture.h"

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

// Capture watch ids of the GPU Scene window, in the order of gpuSceneCaptures().
inline constexpr uint32_t gpuSceneWatchBase = 2000;

struct GpuSceneWindowState {
    bool live = false;
    uint32_t trigger = 1;
    std::map<std::string, CaptureResult> results; // by resource
    int cullView = 0;
    int compactionRegion = 0;
    char filter[64] = "";
};

// The GPU scene tables as browsable data, culling per view with reasons, and the compaction.
// primPath names a prim; selectPrim is called with a prim index when a row is clicked.
auto drawGpuSceneWindow(bool& show,
                        GpuSceneWindowState& state,
                        uint32_t viewCount,
                        std::span<const uint32_t> primOfInstance,
                        const std::function<std::string(uint32_t)>& primPath,
                        const std::function<void(uint32_t)>& selectPrim) -> void;
auto gpuSceneWatches(const GpuSceneWindowState& state) -> std::vector<CaptureWatch>;
auto gpuSceneResourceOfWatch(uint32_t id) -> const std::string*;
