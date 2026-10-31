#pragma once

#include "debugcamera.h"
#include "lodtool.h"
#include "streamingtool.h"

// The tools' state (src/tools/README.md): the LOD and Streaming tools' data and requests, and the debug camera. The application owns it and
// fills it from the engine; the UI's windows draw and edit it through a reference.
struct ToolsState {
    LodToolState lod;
    StreamingToolState streaming;
    DebugCameraState debugCamera;
};
