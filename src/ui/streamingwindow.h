#pragma once

#include "debugview.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// One level of a streaming root as the Streaming window shows it: its authored name, whether the root asks for it to be resident, it is
// loaded (composed), ready (placed and waiting for nothing) and drawn, and its last lead in seconds (-1 never selected).
struct StreamingWindowLevel {
    std::string name;
    bool requested = false;
    bool loaded = false;
    bool ready = false;
    bool drawn = false;
    double lead = -1.0;
};

// One streaming root as the Streaming window lists it (the `streaming` record's data per root): its levels, the level selected to draw,
// its state (the finest level that must be resident) and the levels resident, the level drawn (-1 for none), its streaming measure and radii
// in force (metres for a distance), its state for the colours, its authoring findings, and where it is for the map and the overlays: its
// streaming bounds in world metres.
struct StreamingWindowRow {
    std::string path;
    std::string name;  // the last part of the path
    uint32_t slot = 0; // its streaming root slot + 1 (the streaming view's), 0 for none
    std::vector<StreamingWindowLevel> levels;
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
    StreamingWindowRow row;
    std::string stateWhy; // which radius put the state where it is
    std::vector<Heuristic> heuristics;
    std::string parent;
    std::vector<std::string> children;
};

// Totals of the `streaming` record's `measured` block, the scene thread's assets in flight, and a short history for the strip chart.
struct StreamingWindowTotals {
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

// Persistent UI state of the Streaming window.
struct StreamingWindowState {
    char filter[128] = "";
    bool onlyLate = false;
    bool onlyHoles = false;
    bool onlyPrefetching = false;
    bool onlyFindings = false;
    bool onlyUnderSelection = false;
    std::vector<StreamingWindowRow> rows; // set by the application when the scene changes
    StreamingWindowTotals totals;         // set by the application each frame
    std::string selectedRoot;             // the streaming root at or above the view's selection; set by the application
    std::string selectedPath;
    std::string scrolledTo;
    std::optional<StreamingInspection> inspection; // the selected root's; set by the application
    // Per streaming root slot, its state (StreamingRootState and streamingStateFinding) for the streaming view; built with the rows.
    std::vector<uint32_t> slotStates;
    // The strip chart: a sample per frame of assets in flight, roots late and roots drawing nothing.
    static constexpr size_t historyLength = 600;
    std::array<float, historyLength> inFlightHistory = {};
    std::array<float, historyLength> lateHistory = {};
    std::array<float, historyLength> holeHistory = {};
    size_t historyAt = 0;
    // The map: where it looks (world metres, in the plane across the camera's up), metres per pixel, and whether it follows the camera.
    glm::vec2 mapCenter = glm::vec2(0.0f);
    float mapMetresPerPixel = 4.0f;
    bool mapFollow = true;
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
    // Edit buffers for the inspector's radii, reset when the inspected root changes.
    std::string editedRoot;
    std::array<char, 256> loadText = {};
    std::array<char, 256> unloadText = {};
};

// A streaming state's colour, as the streaming view's (shaders/debugview.comp): none grey, drawn green, prefetched teal, requested blue,
// refused violet, late orange, hole red.
auto streamingStateColor(StreamingRootState state) -> glm::vec4;
auto streamingStateName(StreamingRootState state) -> const char*;
// Whether a row passes the list's filters.
auto streamingRowShown(const StreamingWindowState& state, const StreamingWindowRow& row) -> bool;

// The Streaming window: an overview (counters, the strip chart), the roots (filtered; click selects, double-click frames), the inspector of
// the selected root (its heuristics by domain, radii authored and in force, editable; its levels; its state and why; findings), the
// measurement controls (reset, fly a camera path), the viewport's layers, and the map of the roots around the camera.
auto drawStreamingWindow(bool& show, StreamingWindowState& state) -> void;
// The labels layer: over each root the list shows, its name, drawn / selected level, state and its state's name; a click selects it.
auto drawStreamingLabels(StreamingWindowState& state, const glm::mat4& viewProj, glm::vec2 viewport) -> void;
