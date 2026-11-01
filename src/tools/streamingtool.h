#pragma once

#include "config.h"
#include "debugview.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class DebugDraw;
class ViewScene;

// The Streaming tool's data (src/view/README.md, "The Streaming tool"): what the Streaming window lists, inspects, maps and draws in the
// viewport, built from the scene's snapshot: the streaming roots placed, their residency and what streaming measured.

// One level of a streaming root as the Streaming tool holds it: its authored name, whether the root asks for it to be resident, it is
// loaded (composed), ready (placed and waiting for nothing) and drawn, and its last lead in seconds (-1 never selected).
struct StreamingLevel {
    std::string name;
    bool requested = false;
    bool loaded = false;
    bool ready = false;
    bool drawn = false;
    double lead = -1.0;
};

// One streaming root as the Streaming tool lists it (the `streaming` record's data per root): its levels, the level selected to draw,
// its state (the finest level that must be resident) and the levels resident, the level drawn (-1 for none), its streaming measure and radii
// in force (metres for a distance), its state for the colours, its authoring findings, and where it is for the map and the overlays: its
// streaming bounds in world metres.
struct StreamingRow {
    std::string path;
    std::string name;  // the last part of the path
    uint32_t slot = 0; // its streaming root slot + 1 (the streaming view's), 0 for none
    std::vector<StreamingLevel> levels;
    int32_t selected = -1;
    int32_t state = -1;
    int32_t drawn = -1;
    std::vector<uint32_t> resident;
    std::string why;         // why it selects its level
    std::string residentWhy; // why these levels are resident
    std::string key;         // its authored root, which the radii's tuning names
    int32_t streamingHeuristic = -1;
    std::string kind; // the streaming heuristic's: "distance" or "screen"
    float measure = -1.0f;
    std::vector<float> load;
    std::vector<float> unload;
    bool tuned = false;
    StreamingRootState rootState = StreamingRootState::None;
    bool late = false;
    bool hole = false;
    uint32_t reloads = 0;
    std::vector<std::string> findings;
    bool hasBounds = false;
    glm::vec3 boundsMin = glm::vec3(0.0f);
    glm::vec3 boundsMax = glm::vec3(0.0f);
};

// The inspector's view of one streaming root: what the row has, and every heuristic it names by domain, with its thresholds authored and in
// force, why it is or isn't used, and how the state came about; its parent and children roots.
struct StreamingInspection {
    struct Heuristic {
        std::string domain; // "imaging", "streaming", or the domain authored
        std::string kind;   // "distance", "screen", or the prim type of one the view can't read
        std::string path;
        bool used = false;
        std::string why;
        std::vector<float> authored;
        std::vector<float> authoredUnload;
        std::vector<float> thresholds; // in force
        std::vector<float> unload;     // in force
        float measure = -1.0f;
    };
    StreamingRow row;
    std::string stateWhy; // which radius put the state where it is
    std::vector<Heuristic> heuristics;
    std::string parent;
    std::vector<std::string> children;
};

// Totals of the `streaming` record's `measured` block, the scene thread's assets in flight, and a short history for the strip chart.
struct StreamingTotals {
    uint32_t roots = 0;
    uint32_t lateNow = 0;
    uint32_t holesNow = 0;
    uint64_t lateSwitches = 0;
    uint64_t holes = 0;
    double lateSeconds = 0.0;
    uint64_t loads = 0;
    uint64_t reloads = 0;
    uint64_t released = 0;
    uint64_t leads = 0;
    double leadMin = 0.0;
    double leadMedian = 0.0;
    uint64_t arrivedUnwanted = 0;
    uint32_t findings = 0;
    uint32_t inFlight = 0;       // assets the scene thread waits for
    uint32_t residentLevels = 0; // levels the roots ask to be resident
};

// The Streaming tool's state: what the Streaming window lists, inspects and maps (the application builds the rows, totals and inspection
// from the scene), the list's filters (the layers of every root the list shows read them), the selection, the strip chart's history, the
// cameras for the map, the flight, and what the window asked for this frame (the application applies the requests and clears them). The
// window's own state (scroll, the map's pan and zoom, edit buffers) is the UI's.
struct StreamingToolState {
    bool windowOpen = false; // the Streaming window is open; set by the UI each frame. The rows, inspector and chart are kept while it is.
    char filter[128] = "";
    bool onlyLate = false;
    bool onlyHoles = false;
    bool onlyPrefetching = false;
    bool onlyFindings = false;
    bool onlyUnderSelection = false;
    std::vector<StreamingRow> rows; // set by the application when the scene changes
    StreamingTotals totals;         // set by the application each frame
    std::string selectedRoot;       // the streaming root at or above the view's selection; set by the application
    std::string selectedPath;
    std::optional<StreamingInspection> inspection; // the selected root's; set by the application
    // Per streaming root slot, its state (StreamingRootState and streamingStateFinding) for the streaming view; built with the rows.
    std::vector<uint32_t> slotStates;
    // The strip chart: a sample per frame of assets in flight, roots late and roots drawing nothing.
    static constexpr size_t historyLength = 600;
    std::array<float, historyLength> inFlightHistory = {};
    std::array<float, historyLength> lateHistory = {};
    std::array<float, historyLength> holeHistory = {};
    size_t historyAt = 0;
    // The camera, for the map; set by the application. The main camera's, which residency follows; the debug camera's when it is on.
    glm::vec3 cameraPosition = glm::vec3(0.0f);
    std::optional<glm::vec3> debugCameraPosition;
    glm::vec3 cameraForward = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 up = glm::vec3(0.0f, 0.0f, 1.0f);
    // The flight: set by the application while one runs.
    bool flying = false;
    std::string flightText;
    char flightPrefix[128] = "/cameras/flight_";
    float flightSpeed = 30.0f;
    // The viewport's layers: the streaming view (the debug view the application sets), the roots' bounds in their state's colour, the
    // selected root's (or every root's) load and unload rings, labels.
    bool streamingView = false; // set by the application
    std::optional<bool> streamingViewRequested;
    // What the window asked for this frame: the application applies them and clears these.
    std::optional<std::string> selectRequested;
    std::optional<std::string> frameRequested;
    bool resetRequested = false;
    bool flyRequested = false;
    bool flyStopRequested = false;
    struct RadiiRequest {
        std::string root;
        uint32_t heuristic = 0;
        std::optional<std::vector<float>> load;
        std::optional<std::vector<float>> unload;
    };
    std::vector<RadiiRequest> radiiRequests;
    std::vector<std::string> radiiResets; // a root's overrides dropped; an empty path drops every one
};

// A streaming state's colour, as the streaming view's (shaders/debugview.comp): none grey, drawn green, prefetched teal, requested blue,
// refused violet, late orange, hole red.
auto streamingStateColor(StreamingRootState state) -> glm::vec4;
auto streamingStateName(StreamingRootState state) -> const char*;
// Whether a row passes the list's filters.
auto streamingRowShown(const StreamingToolState& state, const StreamingRow& row) -> bool;

// The rows, from a snapshot: every streaming root drawn, its state for the colours, its radii in force and its bounds in world metres; the
// totals; and per streaming root slot its state for the streaming view.
auto buildStreamingRows(const ViewScene& scene, StreamingToolState& state) -> void;
// One root inspected: the streaming root at `path`, its heuristics by domain measured from `eye` (world metres; a screen size over the view
// window `windowArea`); none when no streaming root is placed there.
auto inspectStreamingRoot(const ViewScene& scene, const std::string& path, const glm::vec3& eye, float windowArea) -> std::optional<StreamingInspection>;
// The viewport's layers: each shown root's bounds in its state's colour, and the load (solid) and unload (faint) radii of the selected root,
// or of every shown root, per boundary around its bounds, in the plane across `up` through the bounds' middle.
auto drawStreamingOverlays(const StreamingToolState& state, DebugDraw& draw, const glm::vec3& up) -> void;

// The Streaming tool's configuration values (src/config/README.md): its viewport layers.
namespace toolsconfig {
extern config::Bool streamingBounds;   // tools.streaming.bounds
extern config::Bool streamingRings;    // tools.streaming.rings: the selected root's
extern config::Bool streamingRingsAll; // tools.streaming.ringsAll: every shown root's
extern config::Bool streamingLabels;   // tools.streaming.labels
} // namespace toolsconfig
