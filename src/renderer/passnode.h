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

// A barrier the graph issued before a pass: which resource, from which access to which.
struct FgBarrierRecord {
    uint32_t resourceIndex = 0;
    bool buffer = false;
    FgAccessFlags oldAccess = FgAccessFlags::None;
    FgAccessFlags newAccess = FgAccessFlags::None;
    RhiTextureState oldTextureState = RhiTextureState::Undefined;
    RhiTextureState newTextureState = RhiTextureState::Undefined;
    RhiBufferState oldBufferState = RhiBufferState::Undefined;
    RhiBufferState newBufferState = RhiBufferState::Undefined;
};

struct PassNode {
    const char* name = nullptr;
    uint32_t profileNameId = 0; // interned pass name for the CPU record zone
    std::vector<FgResourceAccess> reads;
    std::vector<FgResourceAccess> writes;
    bool hasSideEffects = false;
    bool culled = false;
    RhiCommandStats stats;                  // delta recorded around execute
    std::vector<FgBarrierRecord> barriers;  // issued before this pass in the last execute
    std::vector<RhiCommandRecord> commands; // this pass's slice of the command log, when logging
    std::function<void(FrameGraphContext&)> execute;
};
