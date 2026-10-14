#pragma once

#include "enginemap.h"

#include <functional>
#include <string>

struct EngineMapWindowState {
    std::string selected = "render.uploads";
    EngineMapStats stats;         // set by the application each frame while the window is open
    bool scrollToSelected = true; // the map opens scrolled to the selected node
    bool focusWindow = false;     // a "Map" link asked for it: bring the window to the front
};

// The Engine Map: a lane per process or thread, the engine's parts in the order data flows, arrows for what flows between them. The
// selected node's arrows are labelled; below the map it says what the node is, what it receives and hands on, its live numbers, records,
// windows (openWindow opens one by name) and README.
auto drawEngineMapWindow(bool& show, EngineMapWindowState& state, const std::function<void(const char*)>& openWindow) -> void;
