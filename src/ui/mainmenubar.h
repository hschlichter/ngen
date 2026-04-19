#pragma once

#include "scenehandles.h"

#include <string>

class SceneQuerySystem;
class SceneUpdater;
class USDScene;
struct Camera;
struct SDL_Window;

struct MainMenuBarState {
    bool& showSceneWindow;
    bool& showPropertiesWindow;
    bool& showLayersWindow;
    bool& showToolsWindow;
    bool& showUndoWindow;
    bool& showGrid;
    bool& showOrigin;
    bool& showGizmo;
    bool& showAABBs;
    bool& showSelectedAABB;
    bool& showLightGizmos;
    int& gbufferView;
    bool& showBufferOverlay;
    bool& showShadowOverlay;
    bool& showFrameGraph;
    bool& requestQuit;
    std::string& pendingOpenPath;
    SDL_Window* window;
    bool sceneOpen;
    SceneUpdater* sceneUpdater;         // for Edit > Undo/Redo
    USDScene* usdScene;                 // for Edit > Select Parent
    const SceneQuerySystem* sceneQuery; // for Edit > Frame Selected (read-only)
    Camera* camera;                     // for Edit > Frame Selected
    PrimHandle* selectedPrim;           // mutated by Edit > Select Parent
};

void drawMainMenuBar(MainMenuBarState& state);
