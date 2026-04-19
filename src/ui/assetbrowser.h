#pragma once

#include <string>
#include <vector>

// Reusable ImGui widget that enumerates usd-family files (.usd/.usda/.usdc/.usdz) under
// a directory and lets the user pick one. The selected path is relative to `rootDir`,
// which USD's asset resolver handles when referenced from the root layer. Used by the
// Layers window's sublayer picker and (planned) the prim-creation reference dialog.
struct AssetBrowserState {
    std::string rootDir;                  // absolute filesystem root to scan
    std::string selected;                  // path relative to rootDir; empty = nothing picked
    std::vector<std::string> cachedFiles; // relative paths, alphabetically sorted
    bool dirty = true;                    // next draw re-scans before rendering the list
};

// Draw the browser into a fixed-height scrollable child. Returns true on the frame the
// selection changed. Handles the ↻ refresh button and lazy rescan when dirty is set.
bool drawAssetBrowser(AssetBrowserState& state, float height);

// Force a rescan on the next draw. Call after scene open, or from an external refresh
// trigger (e.g. a file appeared on disk).
void invalidateAssetBrowser(AssetBrowserState& state);
