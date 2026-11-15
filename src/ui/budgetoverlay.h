#pragma once

#include "cvar.h"

struct RenderFrameCommands;

// The frame budget overlay, for content developers: the GPU's frame graph passes summed by group (shadows, geometry, lighting,
// post-processing, …) as bars against the frame budget (profile.budget.frameMs), each group's passes beneath it, the whole frame and the
// GPU frame in the header, and the frame's draws and triangles. First under the viewport's corner gizmo, dragged anywhere on it. The numbers
// are the profiler's over the last 60 frames, the zones record's GPU track over the same window.
auto drawBudgetOverlay(const RenderFrameCommands& commands) -> void;

// The UI's cvars (src/cvar/README.md).
namespace uicvars {
extern cvar::Bool budgetOverlay; // ui.overlay.budget
} // namespace uicvars
