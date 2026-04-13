#pragma once

#include "editcommand.h"

#include <optional>

class USDScene;
class SceneQuerySystem;
class MaterialLibrary;

// Persistent state for the Properties panel that needs to live across frames
// (between drag-start and drag-end of a slider). Owned by EditorUI; passed by
// reference into drawPropertiesWindow each frame.
struct PropertiesWindowState {
    // Snapshot of the prim's local transform captured the frame the user
    // grabs a transform slider, used as the inverse hint on the commit edit
    // so undo records the pre-operation state, not the post-Preview cache.
    std::optional<Transform> preEditLocal;
    PrimHandle preEditPrim;
};

void drawPropertiesWindow(bool& show,
                          bool editingBlocked,
                          USDScene& usdScene,
                          PrimHandle selectedPrim,
                          const SceneQuerySystem& sceneQuery,
                          const MaterialLibrary& matLib,
                          std::vector<SceneEditCommand>& pendingEdits,
                          PropertiesWindowState& state);
