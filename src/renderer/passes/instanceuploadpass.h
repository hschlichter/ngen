#pragma once

#include "framegraph.h"
#include "rhitypes.h"

#include <vector>

// Copies the changed instance records, packed in a staging buffer, to their slots in the persistent instance buffer: one region per run
// of consecutive slots, in one copy. Returns the instance handle the draw passes read.
auto addInstanceUploadPass(FrameGraph& fg, FgBufferHandle staging, FgBufferHandle instances, std::vector<RhiBufferCopy> regions) -> FgBufferHandle;
