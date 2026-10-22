#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// One LOD root as the LOD window lists it: its full path, its levels' names finest first, the level it defaults to, wants and draws (-1
// while none is loaded), the levels loaded, and whether its level was set by hand.
struct LodWindowRow {
    std::string path;
    std::vector<std::string> levels;
    std::vector<std::vector<std::string>> levelPayloads; // per level, its payloads' asset ids
    std::string heuristic;                               // "LODScreenSizeHeuristic [0.25, 0.125]", empty for none
    uint32_t defaultIndex = 0;
    uint32_t wanted = 0;
    int32_t drawn = -1;
    std::vector<uint32_t> loaded;
    bool setByHand = false;
    float screenSize = -1.0f; // the heuristic's measure, -1 when not computed
    std::string why;          // why it wants its level: "heuristic", "hand-set", "heuristic off", ...
    std::string waiting;      // while its wanted level is loaded and not drawn: what that level waits for
    // The loaded levels it doesn't want, kept for the unload delay: each with when the delay runs out.
    std::vector<std::pair<uint32_t, std::chrono::steady_clock::time_point>> kept;
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
    bool heuristic = true;                  // the LOD heuristic's state; set by the application
    std::optional<bool> heuristicRequested; // the toggle was clicked
    float unloadDelay = 10.0f;              // seconds; set by the application
    std::optional<float> unloadDelayRequested;
    // Render LOD, the roots whose levels the GPU picks per placement: the groups placed, and from the latest readback the groups drawing
    // each level, falling back while the level they want uploads, at a forced level, and the slots of levels not drawn; the groups under
    // the view's selection. Set by the application.
    struct RenderLod {
        uint32_t groups = 0;
        std::array<uint32_t, 8> drawing = {};
        uint32_t fallbacks = 0;
        uint32_t forced = 0;
        uint32_t slotsNotDrawn = 0;
        uint32_t underSelection = 0;
    };
    RenderLod renderLod;
    // A level for every group under the selection, or (the inner value empty) back to their defaults: the application applies and clears it.
    std::optional<std::optional<uint32_t>> renderLodRequested;
    int renderLodChoice = 0;
};

// The scene's LOD roots: each with its levels, the level it wants (a choice per row: its default or a level) and the level it draws, the
// levels loaded, and the heuristic's screen size and why the root wants its level. A row whose drawn level isn't the one it wants is
// loading it. Clicking a row selects the root; a double-click frames it. "Set shown" sets every row the filter shows to its default, its
// coarsest level or one level (clamped to each root's levels). The row of the root at or above the view's selection is highlighted. The
// Heuristic toggle turns the LOD heuristic on or off; Unload delay sets how long what stopped being drawn is kept. The Render LOD section
// counts what the GPU picks for the render-LOD roots, and sets a level for those under the selection.
auto drawLodWindow(bool& show, LodWindowState& state) -> void;
