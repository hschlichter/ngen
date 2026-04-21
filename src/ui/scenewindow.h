#pragma once

#include "assetbrowser.h"
#include "editcommand.h"
#include "usdscene.h"

struct RenderWorld;

// Persistent state for the Scene window that lives across frames. Owned by
// EditorUI; passed by reference each frame.
struct SceneWindowState {
    // Last selectedPrim observed by drawSceneWindow; when it differs from the
    // current selectedPrim, the window auto-scrolls to the new selection.
    PrimHandle lastSelectedPrim;

    // Reference creation modal. `openReferenceDialog` is a one-shot flip set
    // when the user picks Add Child → Reference… — the next draw opens the
    // modal and clears it. `referenceParent` is the parent path chosen at
    // that moment. `referenceBrowser` is a self-contained browser so the
    // modal can scroll without leaking state into the Asset Browser window.
    bool openReferenceDialog = false;
    std::string referenceParent;
    AssetBrowserState referenceBrowser;
};

void drawSceneWindow(bool& show,
                     bool editingBlocked,
                     USDScene& usdScene,
                     const RenderWorld& renderWorld,
                     PrimHandle& selectedPrim,
                     SceneWindowState& state,
                     std::vector<SceneEditCommand>& pendingEdits);
