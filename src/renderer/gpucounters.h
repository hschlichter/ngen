#pragma once

#include "rhitypes.h"

#include <cstdint>
#include <string>
#include <vector>

// One frame's GPU counters: every timing zone (passes at depth 0, what they timed inside
// nested) and, while pipeline statistics are on, the work each pass made the GPU do.
struct GpuCounters {
    struct Zone {
        std::string name;
        uint16_t depth = 0;
        double startMs = 0.0; // from the first zone's start
        double ms = 0.0;
    };
    struct PassStats {
        std::string name;
        RhiPipelineStats stats;
    };

    uint64_t frame = 0;
    double gpuFrameMs = 0.0;
    uint32_t width = 0; // render resolution, for per-pixel ratios
    uint32_t height = 0;
    std::vector<Zone> zones;
    std::vector<PassStats> passes; // empty when pipeline statistics are off or unsupported
};

auto writeGpuCountersJson(const char* path, const GpuCounters& counters) -> bool;
