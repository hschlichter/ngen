#pragma once

#include <cstdint>

// One logged draw, filled by passes through FrameGraphContext::logDraw while the render
// debugger is open. Indirect passes log the commands they wrote (docs/plan_indirect_draws.md).
struct FgDrawRecord {
    const char* pass = "";
    uint32_t drawIndex = 0; // within the pass
    uint32_t instance = UINT32_MAX;
    uint32_t mesh = 0;
    uint32_t material = 0;
    uint32_t prim = 0;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
};
