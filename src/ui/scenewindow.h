#pragma once

#include "usdscene.h"

struct RenderWorld;

// Persistent state for the Scene window that lives across frames. Owned by
// EditorUI; passed by reference each frame.
struct SceneWindowState {
    // Last selectedPrim observed by drawSceneWindow; when it differs from the
    // current selectedPrim, the window auto-scrolls to the new selection.
    PrimHandle lastSelectedPrim;
};

void drawSceneWindow(bool& show, bool editingBlocked, USDScene& usdScene, const RenderWorld& renderWorld, PrimHandle& selectedPrim, SceneWindowState& state);
