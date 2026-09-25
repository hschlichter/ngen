#pragma once

#include "framegraphdebug.h"
#include "rhitypes.h"

#include <cstdint>
#include <string>
#include <vector>

class RhiDevice;

// One frame as the frame debugger sees it: every executed pass with its accesses, the
// barriers the graph issued before it (with what they became in the backend), the commands
// it recorded, and every descriptor set it bound with their contents.
struct FrameDebugBarrier {
    std::string resource;
    bool buffer = false;
    std::string oldAccess;
    std::string newAccess;
    std::string oldState;
    std::string newState;
    RhiTransitionInfo backend;
};

struct FrameDebugAccess {
    std::string resource;
    std::string access;
};

struct FrameDebugPass {
    std::string name;
    uint32_t order = 0;
    bool culled = false;
    double gpuMs = -1.0;
    RhiCommandStats stats;
    std::vector<FrameDebugAccess> reads;
    std::vector<FrameDebugAccess> writes;
    std::vector<FrameDebugBarrier> barriers;
    std::vector<std::string> commands;
    std::vector<std::string> descriptorSets; // names of the sets bound, in bind order
};

struct FrameDebugSet {
    std::string name;
    std::vector<RhiDescriptorInfo> descriptors;
};

struct FrameDebugCapture {
    uint64_t frame = 0;
    std::vector<FrameDebugPass> passes; // execution order
    std::vector<FrameDebugSet> sets;
};

// Built from a frame-graph debug snapshot of a frame recorded with the command log on.
auto buildFrameDebugCapture(const FrameGraphDebugSnapshot& snap, const RhiDevice& device, uint64_t frame) -> FrameDebugCapture;
auto writeFrameDebugJson(const char* path, const FrameDebugCapture& capture) -> bool;
