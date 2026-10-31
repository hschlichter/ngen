#pragma once

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// What a LOD root's heuristic measures, placed in the world, for the viewport's gizmo and labels: a distance's center or box (in the root's
// space, `toWorld` placing it) or a screen size's sphere (world metres), and the thresholds the view picks with (tuned), a distance's in
// metres.
struct LodShape {
    enum class Kind : uint8_t {
        None,
        Distance,
        ScreenSize,
    };
    Kind kind = Kind::None;
    glm::mat4 toWorld = glm::mat4(1.0f);
    glm::vec3 point = glm::vec3(0.0f);
    bool hasBox = false;
    glm::vec3 boxMin = glm::vec3(0.0f);
    glm::vec3 boxMax = glm::vec3(0.0f);
    glm::vec3 center = glm::vec3(0.0f);
    float radius = 0.0f;
    std::vector<float> thresholds;
    std::vector<float> blendThresholds;

    // Where the root's label and the gizmo's rings sit: the distance's center (the box's middle), or the sphere's center.
    auto anchor() const -> glm::vec3;
};

// One LOD root as the LOD window lists it, either kind: a streaming root (its levels payloads, chosen per root on the CPU) or a render-LOD
// group (every level held, chosen per placement on the GPU). Its full path, its levels' names finest first, the level it defaults to, wants
// and draws (-1 while none is loaded or read back), the level fading in and its share, the levels loaded, whether its level was set by hand.
struct LodWindowRow {
    std::string path;
    int32_t group = -1; // the render-LOD group slot; -1 for a streaming root
    uint32_t depth = 1; // how many LOD roots it is in, itself counted: the hue of its swatches
    std::vector<std::string> levels;
    std::vector<std::vector<std::string>> levelPayloads; // per level, its payloads' asset ids
    std::string heuristic;                               // every heuristic it names, the one used marked: "distance [49.3, 70.8]* | screen [0.25]"
    std::string key;                                     // its authored root, which threshold overrides name
    bool overridden = false;                             // the view's thresholds replace the authored ones for its authored root
    uint32_t defaultIndex = 0;
    uint32_t wanted = 0;
    int32_t drawn = -1;
    int32_t fadeLevel = -1; // the level fading in, -1 for none
    float fade = 0.0f;      // its share
    bool flashing = false;  // a render-LOD group's pop flash is on
    std::vector<uint32_t> loaded;
    bool setByHand = false;
    float measure = -1.0f;   // the heuristic's measure, -1 when not computed
    std::string measureUnit; // "m" for a distance (the stage's units), empty for a screen size
    std::string why;         // why it wants its level: "distance", "screen size", "hand-set", "heuristic off", ...
    std::string waiting;     // while its wanted level is loaded and not drawn: what that level waits for
    // A streaming root's residency: the state (the finest level that must be resident) and why, from the streaming heuristic's radii.
    std::optional<uint32_t> state;
    std::string residentWhy;
    LodShape shape;
};

// The inspector's view of one root: what LodWindowRow has, and per heuristic and per level what the list leaves out.
struct LodInspection {
    struct Heuristic {
        std::string kind; // "distance", "screen", or the prim type of one the view can't read
        std::string path; // its prim's path in the root's content
        bool used = false;
        std::string why;                    // why it is or isn't the one used
        std::vector<float> authored;        // thresholds as authored
        std::vector<float> authoredBlends;  // blend thresholds as authored
        std::vector<float> thresholds;      // in force: the override's, else the authored (before the bias)
        std::vector<float> blendThresholds; // likewise
        bool overridden = false;            // an override of this heuristic is in force
        float measure = -1.0f;              // from the camera now, in its unit (the stage's units for a distance); -1 when it can't measure
        int32_t picks = -1;                 // the level it would want from the camera now, under the tuning (from the finest level)
        float blend = 0.0f;                 // and the next level's share inside a band
    };
    struct Level {
        std::string name;
        uint64_t triangles = 0; // of its draws; for a streaming root, only the drawn level's are known
        uint32_t draws = 0;
        bool known = false; // its draws are counted
        bool loaded = false;
    };
    std::string path;
    uint32_t depth = 1; // how many LOD roots it is in, itself counted: the hue of its swatches
    bool renderLod = false;
    int32_t group = -1;
    std::string key;
    uint32_t defaultIndex = 0;
    int32_t wanted = -1;
    int32_t drawn = -1;
    int32_t fadeLevel = -1;
    float fade = 0.0f;
    bool setByHand = false;
    bool forced = false; // hand-set, or the default with the heuristic off or no usable heuristic
    std::string why;
    std::vector<Heuristic> heuristics;
    std::vector<Level> levels;
    float metersPerUnit = 1.0f; // the stage's: a distance's unit
};

// Persistent UI state of the LOD window.
struct LodWindowState {
    char filter[128] = "";
    int kindFilter = 0; // 0 every root, 1 render LOD, 2 streaming
    bool onlyUnderSelection = false;
    bool onlyNotDrawingWanted = false;
    bool onlyFading = false;
    std::vector<LodWindowRow> rows; // set by the application when the scene changes; render-LOD rows' live state each frame
    // What the render-LOD group rows (after the LOD roots' rows in `rows`) were built from: the snapshot's draws (their revision), the tuning
    // and the hand-set levels (held, so a new one can't take a freed one's address), the preference and the heuristic's state. A snapshot
    // that changes none of them (a camera move) rebuilds only the roots' rows.
    struct GroupRowsSource {
        uint64_t drawsRevision = UINT64_MAX;
        std::shared_ptr<const void> tuning;
        std::shared_ptr<const void> handLevels;
        int preference = -1;
        bool heuristic = false;
        auto operator==(const GroupRowsSource&) const -> bool = default;
    };
    GroupRowsSource groupRowsSource;
    size_t rootRowCount = 0;
    std::string selectedRoot; // the root at or above the view's selected prim; set by the application each frame
    std::string selectedPath; // the view's selection, for "under selection"
    std::string scrolledTo;   // the selected root the list last scrolled to
    int allChoice = 0;        // the "Set shown" choice: 0 default, 1 coarsest, 2 + n level n
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
    int preference = 0;                     // which heuristic a root naming several measures by: 0 distance first, 1 screen size first, 2 authored order
    std::optional<int> preferenceRequested;
    // The tuning (the view's LodTuning): set by the application; a change asked for in the matching request.
    float bias = 1.0f;
    float hysteresis = 0.2f;
    bool blending = true;
    uint32_t overrides = 0; // authored roots with overridden thresholds
    std::optional<float> biasRequested;
    std::optional<float> hysteresisRequested;
    std::optional<bool> blendingRequested;
    struct OverrideRequest {
        std::string root;
        uint32_t heuristic = 0;
        std::optional<std::vector<float>> thresholds;
        std::optional<std::vector<float>> blendThresholds;
    };
    std::vector<OverrideRequest> overrideRequests;
    std::vector<std::string> overrideResets; // a root's overrides dropped; an empty path drops every one
    // The viewport's layers: the LOD Level view (the G-buffer view the application sets), labels over the roots, the heuristic gizmo of the
    // selected root or of every root the list shows, and the pop flash.
    bool levelView = false; // set by the application
    std::optional<bool> levelViewRequested;
    bool labels = false;
    bool gizmo = false;
    bool gizmoAll = false;
    bool flash = false;
    // The LOD Level view's colour (and the window's swatches): by the drawn level (an index within its root), or by how many LOD roots deep.
    bool colorByDepth = false;
    std::optional<bool> triangleSizeRequested;
    // The inspector: the selected root's, refreshed by the application when the selection changes and once a second while shown.
    std::optional<LodInspection> inspection;
    // Render LOD, the roots whose levels the GPU picks per placement: the groups placed, and from the latest readback the groups drawing
    // each level, falling back while the level they want uploads, at a forced level, fading, popping, and the slots of levels not drawn.
    // Set by the application.
    struct RenderLod {
        uint32_t groups = 0;
        std::array<uint32_t, 8> drawing = {};
        uint32_t fallbacks = 0;
        uint32_t forced = 0;
        uint32_t fading = 0; // groups cross-fading two levels inside a blend band
        uint32_t popped = 0; // groups whose drawn level changed without a fade, at the last readback
        uint64_t pops = 0;   // since the scene opened, counted over the readbacks
        uint32_t slotsNotDrawn = 0;
    };
    RenderLod renderLod;
    // Edit buffers for the inspector's threshold fields, by heuristic index; reset when the inspected root changes.
    std::string editedRoot;
    std::vector<std::array<char, 256>> thresholdText;
    std::vector<std::array<char, 256>> blendText;
};

// The LOD Level view's colour of a level of a root `depth` LOD roots deep (itself counted), as lighting.frag's lodLevelTint: by level, 0 red,
// 1 orange, 2 yellow, 3 green, 4 cyan, 5 blue, 6 magenta, 7 white; by depth, 1 blue, 2 green, 3 orange, 4 and more magenta.
auto lodLevelColor(uint32_t level, uint32_t depth, bool byDepth) -> glm::vec4;

// The LOD tool: every LOD root placed, both kinds, in one list (filtered by path, kind, under the selection, not drawing what it wants,
// fading; click selects, double-click frames), and below it the inspector of the selected root: its heuristics (each one's thresholds in
// force, editable, with the authored ones beside; its measure now and the level it would pick), its levels (triangles, draws), and its
// state. The header holds the global controls (heuristic, preference, bias, hysteresis, blending, unload delay) and the viewport's layers
// (the LOD Level view with its legend, labels, the gizmo, the pop flash, the Triangle Size view).
auto drawLodWindow(bool& show, LodWindowState& state) -> void;
// The labels layer: over each root the list shows, inside the viewport, its name, drawn level (→ the level fading in and its share) and
// measure; a click selects it. `viewProj` maps world metres to clip space; `viewport` is in ImGui's display units. Inside the ImGui frame.
auto drawLodLabels(LodWindowState& state, const glm::mat4& viewProj, glm::vec2 viewport) -> void;
// Whether a row passes the list's filters.
auto lodRowShown(const LodWindowState& state, const LodWindowRow& row) -> bool;
