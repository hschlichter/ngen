#pragma once

#include "framegraphresource.h"
#include "rhitypes.h"

#include <functional>
#include <vector>

class FrameGraphContext;

struct FgResourceAccess {
    uint32_t resourceIndex;
    FgAccessFlags access;
};

struct PassNode {
    const char* name = nullptr;
    uint32_t profileNameId = 0; // interned pass name for the CPU record zone
    std::vector<FgResourceAccess> reads;
    std::vector<FgResourceAccess> writes;
    bool hasSideEffects = false;
    bool culled = false;
    RhiCommandStats stats; // delta recorded around execute
    std::function<void(FrameGraphContext&)> execute;
};
