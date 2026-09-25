#pragma once

#include "rendersnapshot.h"

#include "renderdebug.h"
#include "scenehandles.h"

#include <optional>

// Editor flags the View tab edits in place; the menu edits the same flags.
struct RenderDebugViewFlags {
    int& gbufferView;
    SamplerSettings& sampler;
    ShadowCascadeSettings& shadow;
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

// Persistent UI state for the draw list and the texture inspector.
struct RenderDebugDrawState {
    TextureInspectRequest inspect; // texture inspector selection, same route
    bool inspectChanged = false;
    bool dumpRequested = false; // "Dump level" pressed: caller writes texture_<material>_L<level>.png
};

// Returns true when the Screenshot button was pressed this frame.
auto drawRenderDebugWindow(bool& show, const std::optional<RenderDebugSnapshot>& snap, RenderDebugViewFlags& view, const USDScene& scene, PrimHandle& selectedPrim, RenderDebugDrawState& drawState) -> bool;
