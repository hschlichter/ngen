#pragma once

#include "config.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class DebugDraw;
class ViewScene;
struct CullResult;

// The LOD tool's data (src/view/README.md, "The LOD tool"): what the LOD window lists, inspects and draws in the viewport, built from the
// scene's snapshot and the render-LOD readback.

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

// One LOD root as the LOD tool lists it, either kind: a streaming root (its levels payloads, chosen per root on the CPU) or a render-LOD
// group (every level held, chosen per placement on the GPU). Its full path, its levels' names finest first, the level it defaults to, wants
// and draws (-1 while none is loaded or read back), the level fading in and its share, the levels loaded, whether its level was set by hand.
struct LodRow {
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

// The inspector's view of one root: what LodRow has, and per heuristic and per level what the list leaves out.
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

// The LOD tool's state: what the LOD window lists and inspects (the application builds the rows and the inspection from the scene), the
// list's filters (the gizmo of every root the list shows reads them), the selection, the render-LOD counts, and what the window asked for
// this frame (the application applies the requests and clears them). The window's own state (scroll, edit buffers) is the UI's.
struct LodToolState {
    bool windowOpen = false; // the LOD window is open; set by the UI each frame. The rows and the inspector are kept while it is.
    char filter[128] = "";
    int kindFilter = 0; // 0 every root, 1 render LOD, 2 streaming
    bool onlyUnderSelection = false;
    bool onlyNotDrawingWanted = false;
    bool onlyFading = false;
    std::vector<LodRow> rows; // set by the application when the scene changes; render-LOD rows' live state each frame
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
};

// The LOD Level view's colour of a level of a root `depth` LOD roots deep (itself counted), as lighting.frag's lodLevelTint: by level, 0 red,
// 1 orange, 2 yellow, 3 green, 4 cyan, 5 blue, 6 magenta, 7 white; by depth, 1 blue, 2 green, 3 orange, 4 and more magenta.
auto lodLevelColor(uint32_t level, uint32_t depth, bool byDepth) -> glm::vec4;

// Whether a row passes the list's filters.
auto lodRowShown(const LodToolState& state, const LodRow& row) -> bool;

// The camera the inspector measures from: its position in world metres, vertical field of view in radians, and aspect.
struct LodToolCamera {
    glm::vec3 eye = glm::vec3(0.0f);
    float fovY = 0.785f;
    float aspect = 1.0f;
};

// The list's rows, from a snapshot: every streaming root drawn and every render-LOD group placed, with their heuristics as the view picks
// with them (tuned) and their shapes for the viewport. The render-LOD rows' live state is updateLodRows'.
auto buildLodRows(const ViewScene& scene, LodToolState& state) -> void;
// The render-LOD rows' live state from the latest readback with states: wanted, drawn, fading, measure, the pop flash.
auto updateLodRows(const ViewScene& scene, const CullResult& cull, LodToolState& state) -> void;
// One root inspected: the LOD root at `path` (a streaming root's or a render-LOD group's full path); none when no root is placed there.
auto inspectLodRoot(const ViewScene& scene, const CullResult& cull, const std::string& path, const LodToolCamera& camera) -> std::optional<LodInspection>;
// The heuristic gizmo: for the selected root, or every root the list shows, its distance's center or box, or its screen size's sphere,
// and a ring (in the plane across `up`) at each threshold, at the distance where it is reached (a screen size's from the camera's window,
// `windowArea`), in the colour of the level beyond it; a fainter ring where each blend band ends.
auto drawLodGizmos(const LodToolState& state, DebugDraw& draw, const glm::vec3& up, float windowArea) -> void;
// The distance unit a stage's metres per unit names: "m", "cm", else "units".
auto lodUnitName(double metersPerUnit) -> const char*;

// The LOD tool's configuration values (src/config/README.md): its viewport layers.
namespace toolsconfig {
extern config::Bool lodLabels;       // tools.lod.labels
extern config::Bool lodGizmo;        // tools.lod.gizmo: the selected root's heuristic
extern config::Bool lodGizmoAll;     // tools.lod.gizmoAll: every shown root's
extern config::Bool lodPopFlash;     // tools.lod.popFlash
extern config::Bool lodColorByDepth; // tools.lod.colorByDepth: the LOD Level view's colours
} // namespace toolsconfig
