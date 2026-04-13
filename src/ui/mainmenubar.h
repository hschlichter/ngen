#pragma once

#include <string>

class SceneUpdater;
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
    int& gbufferView;
    bool& showBufferOverlay;
    bool& requestQuit;
    std::string& pendingOpenPath;
    SDL_Window* window;
    bool sceneOpen;
    SceneUpdater* sceneUpdater; // for Edit > Undo/Redo
};

void drawMainMenuBar(MainMenuBarState& state);
