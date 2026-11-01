#pragma once

#include "debugcamera.h"
#include "lodtool.h"
#include "streamingtool.h"

#include <cstdint>

// What a click and a drag do in the viewport: select, or move the selection with a gizmo.
enum class ViewTool : uint8_t {
    Select,
    Translate,
    Rotate,
    Scale,
};

// The tools' state (src/tools/README.md): the LOD and Streaming tools' data and requests, and the debug camera. The application owns it and
// fills it from the engine; the UI's windows draw and edit it through a reference.
struct ToolsState {
    LodToolState lod;
    StreamingToolState streaming;
    DebugCameraState debugCamera;
    ViewTool activeTool = ViewTool::Select; // the menu's; set by the UI each frame
};
