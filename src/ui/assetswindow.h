#pragma once

#include "assetclient.h"

#include <vector>

// Persistent UI state of the Assets window.
struct AssetsWindowState {
    char filter[128] = "";
    int stateFilter = 0; // 0 all, 1 in flight, 2 received, 3 failed
};

// What the view asked the asset server for: every asset id, its state, whether it was packed or came from the cache, its size, and its
// times (the total from request to arrival, the server's share of it, and the rest: queueing and streaming). From AssetClient::log().
auto drawAssetsWindow(bool& show, const std::vector<AssetLogEntry>& log, AssetsWindowState& state) -> void;
