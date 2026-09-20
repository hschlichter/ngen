#pragma once

#include "renderdebug.h"
#include "scenehandles.h"

#include <optional>

// Editor flags the View tab edits in place; the menu edits the same flags.
struct RenderDebugViewFlags {
    int& gbufferView;
    bool& showBufferOverlay;
    bool& showShadowOverlay;
    bool& antiAliasing;
    bool& showGrid;
    bool& showOrigin;
    bool& showGizmo;
    bool& showAABBs;
    bool& showLightGizmos;
};

class USDScene;
struct PrimHandle;

// Persistent UI state for the draw list: which pass is expanded, the timing window.
struct RenderDebugDrawState {
    FgDrawTimingRequest timing; // edited here, sent to the render thread by the caller
    bool timingChanged = false;
};

void drawRenderDebugWindow(bool& show, const std::optional<RenderDebugSnapshot>& snap, RenderDebugViewFlags& view, const USDScene& scene, PrimHandle& selectedPrim, RenderDebugDrawState& drawState);
