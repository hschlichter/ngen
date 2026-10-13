#pragma once

#include "assetclient.h"

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Persistent UI state of the Assets window.
struct AssetsWindowState {
    char filter[128] = "";
    int stateFilter = 0;            // 0 all, 1 in flight, 2 received, 3 failed
    std::set<std::string> selected; // asset ids
    std::string anchor;             // the row a Shift+click range starts from
    // What the window asked for this frame: the application repacks them and clears these.
    std::vector<std::string> repackRequested;
    bool repackSceneRequested = false;
    // A "go to" asked for this frame: the asset, and whether every draw of it at once. The application answers in goToStatus.
    struct GoTo {
        std::string id;
        bool all = false;
    };
    std::optional<GoTo> goToRequested;
    std::string goToStatus;
    // "Selected prim's assets" was pressed: the application selects the rows of the assets the view's selected prim uses (selectAssets).
    bool findSelectedRequested = false;
    bool showSelectedOnly = false;
    bool hasPrimSelected = false; // the view has a prim selected; set by the application each frame
    // How many draws use an asset; set by the application.
    std::function<size_t(const std::string&)> drawCount;
};

// What the view asked the asset server for: every asset id, its state, whether it was packed or came from the cache, its size, and its
// times (the total from request to arrival, the server's share of it, and the rest: queueing and streaming). From AssetClient::log().
// Clicking a row selects it, Ctrl+click adds or removes a row, Shift+click selects the rows from the last click to it, as sorted and
// filtered; Ctrl+A or Select shown selects every row shown. A double-click or the row menu's Go to moves the camera to the asset's next draw
// (Frame all to every draw); Selected prim's assets selects the rows of what the view's selected prim uses; Repack selected (or the row
// menu) asks for a forced repack of the selection, Repack scene of every asset of the scene.
auto drawAssetsWindow(bool& show, const std::vector<AssetLogEntry>& log, AssetsWindowState& state) -> void;
