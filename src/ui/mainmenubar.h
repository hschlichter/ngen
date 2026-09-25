#pragma once

#include "introspectionflags.h"

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
    bool& depthPrepass;
    int& gbufferView;
    int& debugView; // DebugView
    bool& showBufferOverlay;
    bool& showShadowOverlay;
    bool& antiAliasing;
    bool& showFrameGraph;
    bool& showPerformance;
    bool& showRenderDebug;
    bool& showCamera;
    bool& showCulling;
    IntrospectionFlags& introspection;
    bool& showAssetBrowser;
    bool& requestQuit;
    bool& pendingNewScene;
    std::string& pendingOpenPath;
    std::string& pendingSavePath;
    SDL_Window* window;
    bool sceneOpen;
    SceneUpdater* sceneUpdater;         // for Edit > Undo/Redo
    USDScene* usdScene;                 // for Edit > Select Parent
    const SceneQuerySystem* sceneQuery; // for Edit > Frame Selected (read-only)
    Camera* camera;                     // for Edit > Frame Selected
    PrimHandle* selectedPrim;           // mutated by Edit > Select Parent
};

void drawMainMenuBar(MainMenuBarState& state);
