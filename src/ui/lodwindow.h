#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// One LOD root as the LOD window lists it: its full path, its levels' names finest first, the level it defaults to, wants and draws (-1
// while none is loaded), the levels loaded, and whether its level was set by hand.
struct LodWindowRow {
    std::string path;
    std::vector<std::string> levels;
    uint32_t defaultIndex = 0;
    uint32_t wanted = 0;
    int32_t drawn = -1;
    std::vector<uint32_t> loaded;
    bool setByHand = false;
};

// Persistent UI state of the LOD window.
struct LodWindowState {
    char filter[128] = "";
    std::vector<LodWindowRow> rows; // set by the application when the scene changes
    std::string selectedRoot;       // the root at or above the view's selected prim; set by the application each frame
    std::string scrolledTo;         // the selected root the list last scrolled to
    int allChoice = 0;              // the "Set shown" choice: 0 default, 1 coarsest, 2 + n level n
    // What the window asked for this frame: the application applies them and clears these.
    struct LevelRequest {
        std::string root;
        std::optional<uint32_t> level; // none: back to the root's default
    };
    std::vector<LevelRequest> levelRequests;
    std::optional<std::string> frameRequested;
    std::optional<std::string> selectRequested;
};

// The scene's LOD roots: each with its levels, the level it wants (a choice per row: its default or a level) and the level it draws, and the
// levels loaded. A row whose drawn level isn't the one it wants is loading it. Clicking a row selects the root; a double-click frames it. "Set
// shown" sets every row the filter shows to its default, its coarsest level or one level (clamped to each root's levels). The row of the root
// at or above the view's selection is highlighted.
auto drawLodWindow(bool& show, LodWindowState& state) -> void;
