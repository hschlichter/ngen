#pragma once

#include <cstdint>

// What the status bar shows, gathered by the application each frame. The UI library only draws it.
struct StatusBarData {
    double fps = 0.0;
    double frameMs = 0.0;        // CPU frame interval, averaged over the last second
    double gpuMs = 0.0;          // GPU frame time, averaged over the frames that reported one; 0 = unknown
    uint64_t trianglesDrawn = 0; // by the camera after GPU culling, a few frames old
    uint64_t trianglesScene = 0; // every instance, culled or not
    uint64_t cpuMemoryBytes = 0; // the process's resident set
    uint64_t gpuMemoryBytes = 0; // device memory the engine has allocated, every heap
    bool assetsConnected = false;
    uint64_t assetsRequested = 0;
    uint64_t assetsPacked = 0;
    uint64_t assetsCached = 0;
    uint64_t assetsFailed = 0;
    uint64_t assetsInFlight = 0;
    uint64_t assetBytes = 0;
};

// A bar along the bottom of the main viewport: timings on the left, memory and asset activity on the right.
auto drawStatusBar(const StatusBarData& data) -> void;
