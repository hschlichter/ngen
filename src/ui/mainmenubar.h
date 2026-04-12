#pragma once

#include <string>

struct SDL_Window;

struct MainMenuBarState {
    bool& showSceneWindow;
    bool& showPropertiesWindow;
    bool& showLayersWindow;
    bool& showAABBs;
    bool& showSelectedAABB;
    bool& requestQuit;
    std::string& pendingOpenPath;
    SDL_Window* window;
    bool sceneOpen;
};

void drawMainMenuBar(MainMenuBarState& state);
