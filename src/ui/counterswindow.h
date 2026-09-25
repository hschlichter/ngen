#pragma once

#include "gpucounters.h"

#include <cstdint>
#include <deque>
#include <string>

// Persistent state of the Counters window: the recent frames and the plotted counter.
struct CountersWindowState {
    static constexpr size_t historySize = 300;
    std::deque<GpuCounters> history;
    bool paused = false;
    std::string plotZone = "GeometryPass"; // pass name, or "Pass/zone" for a nested zone
    int plotMetric = 0;                    // countersMetricName index
};

// Number of metrics: GPU time, then the RhiPipelineStats fields in order.
inline constexpr int countersMetricCount = 8;
auto countersMetricName(int metric) -> const char*;
// A metric of a zone in one frame; false when the frame has no such zone or statistic.
auto countersMetricValue(const GpuCounters& frame, const std::string& zone, int metric, double& out) -> bool;

auto addCountersFrame(CountersWindowState& state, GpuCounters frame) -> void;
// Per-pass GPU time and pipeline statistics, the zones nested in each pass, derived ratios,
// and the history of one counter.
auto drawCountersWindow(bool& show, CountersWindowState& state) -> void;
