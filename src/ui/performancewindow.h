#pragma once

#include "profile.h"

#include <cstdint>
#include <vector>

// Timeline state. The view is a window [viewStartNs, viewEndNs] on the CPU clock; all lanes,
// including the GPU, are drawn on that axis (GPU zones are converted by the renderer). Live
// follows the newest data; pausing freezes collection so the view can be panned and zoomed
// over several seconds of history, Tracy-style.
struct PerformanceWindowState {
    bool paused = false;
    bool following = true; // view keeps its right edge on the newest frame
    uint64_t viewStartNs = 0;
    uint64_t viewEndNs = 0;
    double viewSpanNs = 50.0e6; // width when following; zoom changes it

    std::vector<profile::LaneInfo> lanes;
    std::vector<std::vector<profile::Zone>> laneZones;
    std::vector<profile::Zone> gpuZones;
    // Job workers merged into one lane: zones packed into rows, each tagged with its real lane.
    std::vector<profile::Zone> workerZones;
    std::vector<uint32_t> workerZoneLanes;
    uint32_t workerCount = 0;
    std::vector<profile::FrameInterval> frames;
    std::vector<profile::FrameStats> history;
    bool valid = false;

    // Clicked zone, shown in the detail pane. laneIndex UINT32_MAX = GPU lane.
    bool hasSelection = false;
    uint32_t selectedLane = 0;
    profile::Zone selectedZone;
};

void drawPerformanceWindow(bool& show, PerformanceWindowState& state);
