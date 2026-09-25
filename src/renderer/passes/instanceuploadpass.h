#pragma once

#include "framegraph.h"
#include "rhitypes.h"

// Copies the dirty span of the per-slot staging buffer into the persistent instance buffer.
// Staging holds the span at the same offset as the
// instance buffer. Returns the instance handle the draw passes read.
auto addInstanceUploadPass(FrameGraph& fg, FgBufferHandle staging, FgBufferHandle instances, const RhiBufferCopy& region) -> FgBufferHandle;
