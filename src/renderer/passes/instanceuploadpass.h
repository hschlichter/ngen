#pragma once

#include "framegraph.h"
#include "rhitypes.h"

#include <vector>

// Copies regions of a buffer into the persistent instance buffer, in one copy: the changed records packed in a staging buffer to their slots
// (InstanceUpload, one region per run of consecutive slots), or a replaced instance buffer's contents into its successor (InstanceMove).
// `name` is the pass's, a string literal. Returns the instance handle the passes after read.
auto addInstanceUploadPass(FrameGraph& fg, FgBufferHandle source, FgBufferHandle instances, std::vector<RhiBufferCopy> regions, const char* name) -> FgBufferHandle;
