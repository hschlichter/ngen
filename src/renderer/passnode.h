#pragma once

#include "framegraphresource.h"

#include <functional>
#include <vector>

class FrameGraphContext;

struct FgResourceAccess {
    uint32_t resourceIndex;
    FgAccessFlags access;
};

struct PassNode {
    const char* name = nullptr;
    std::vector<FgResourceAccess> reads;
    std::vector<FgResourceAccess> writes;
    bool hasSideEffects = false;
    bool culled = false;
    std::function<void(FrameGraphContext&)> execute;
};
