#pragma once

#include "rendersnapshot.h"

#include "renderdebug.h"
#include "scenehandles.h"

#include <cstdint>
#include <functional>
#include <string>

#include <optional>

// Editor flags the View tab edits in place; the menu edits the same flags.
struct RenderDebugViewFlags {
    int& gbufferView;
    SamplerSettings& sampler;
    ShadowCascadeSettings& shadow;
    AmbientSettings& ambient;
    bool& showBufferOverlay;
    bool& showShadowOverlay;
    bool& antiAliasing;
    bool& showGrid;
    bool& showOrigin;
    bool& showGizmo;
    bool& showAABBs;
    bool& showLightGizmos;
};

// The path of the prim a draw names (its prim index); empty when unknown.
using PrimPathOf = std::function<std::string(uint32_t prim)>;

// Persistent UI state for the draw list and the texture inspector.
struct RenderDebugDrawState {
    TextureInspectRequest inspect; // texture inspector selection, same route
    bool inspectChanged = false;
    bool dumpRequested = false; // "Dump level" pressed: caller writes texture_<material>_L<level>.png
};

// Returns true when the Screenshot button was pressed this frame.
auto drawRenderDebugWindow(bool& show, const std::optional<RenderDebugSnapshot>& snap, RenderDebugViewFlags& view, const PrimPathOf& primPath, uint32_t& selectedPrim, RenderDebugDrawState& drawState) -> bool;
