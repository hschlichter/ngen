#pragma once

#include <cstdint>
#include <string>

// One logged draw, filled by passes through FrameGraphContext::beginDraw while the
// render debugger is open. GPU time arrives later, once the frame's zones are read.
struct FgDrawRecord {
    const char* pass = "";
    uint32_t drawIndex = 0; // within the pass
    uint32_t instance = UINT32_MAX;
    uint32_t mesh = 0;
    uint32_t material = 0;
    uint32_t prim = 0;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    bool timed = false;
    double gpuMs = -1.0;
};

// Which draws to wrap in GPU zones: `count` consecutive draws of `pass` starting at
// `first`. The zone budget per command buffer bounds count.
struct FgDrawTimingRequest {
    bool enabled = false;
    std::string pass;
    uint32_t first = 0;
    uint32_t count = 64;
};
