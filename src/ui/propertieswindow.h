#pragma once

#include "editcommand.h"

#include <glm/glm.hpp>

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

    // Active display-color edit. ColorEdit3 writes to a local each frame, but USD
    // isn't updated until commit — without this cache, the release frame would
    // re-fetch the OLD USD color and commit a no-op. We hold the in-flight color
    // here from first drag until IsItemDeactivatedAfterEdit fires.
    std::optional<glm::vec3> displayColorEdit;
    PrimHandle displayColorEditPrim;
};

void drawPropertiesWindow(bool& show,
                          bool editingBlocked,
                          USDScene& usdScene,
                          PrimHandle selectedPrim,
                          const SceneQuerySystem& sceneQuery,
                          const MaterialLibrary& matLib,
                          std::vector<SceneEditCommand>& pendingEdits,
                          PropertiesWindowState& state);
