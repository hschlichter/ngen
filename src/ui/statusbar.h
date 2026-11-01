#pragma once

#include "profile.h"
#include "rhitypes.h"

#include <cstdint>
#include <vector>

class AssetClient;
class RhiDevice;
struct CullResult;

// What the status bar shows, gathered each frame (StatusBarGatherer).
struct StatusBarData {
    double fps = 0.0;
    double frameMs = 0.0;        // CPU frame interval, averaged over the last second
    double gpuMs = 0.0;          // GPU frame time, averaged over the frames that reported one; 0 = unknown
    uint64_t trianglesDrawn = 0; // by the camera after GPU culling, a few frames old
    uint64_t trianglesScene = 0; // every instance, culled or not
    float cameraSpeed = 0.0f;    // the fly speed of the camera that has the controls, metres a second
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

// Gathers the status bar's numbers each frame: timings from the profiler's frame history, memory from the process and the device (sampled
// four times a second), asset counts from the asset client.
class StatusBarGatherer {
public:
    auto gather(const RhiDevice& device, const AssetClient& assets, const CullResult& cull) -> StatusBarData;

private:
    std::vector<profile::FrameStats> frames;
    std::vector<RhiMemoryHeapInfo> heaps;
    uint64_t lastMemorySampleNs = 0;
    StatusBarData data;
};
