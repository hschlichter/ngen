#pragma once

#include "assetclient.h"
#include "renderdebug.h"

#include <cstdint>
#include <optional>

// The scene thread as the Scene Flow window shows it (the `scenethread` record's numbers).
struct SceneFlowThread {
    uint64_t requestsPosted = 0;
    uint64_t requestsApplied = 0;
    uint64_t batches = 0;
    uint64_t composes = 0;
    double lastComposeMs = 0.0;
    bool composeNeeded = false;
    uint32_t prototypesComposed = 0;
    uint32_t levelsComposed = 0;
    uint32_t prototypesHeld = 0;
    uint32_t levelsHeld = 0;
    uint32_t contentsReached = 0;
    uint64_t extracts = 0;
    double lastExtractMs = 0.0;
    double extractVisitMs = 0.0;
    double extractSlotsMs = 0.0;
    uint64_t extractReused = 0;
    uint64_t extractWalked = 0;
    uint64_t extractKept = 0;
    bool extractNeeded = false;
    uint64_t assetsReceived = 0;
    uint32_t inFlightLayers = 0;
    uint32_t inFlightMeshes = 0;
    uint32_t inFlightMaterials = 0;
    uint32_t inFlightTextures = 0;
    uint32_t inFlightOther = 0;
    uint32_t layerPacks = 0;
    uint32_t meshPacks = 0;
    uint32_t materialPacks = 0;
    uint32_t texturePacks = 0;
    uint32_t failedAssets = 0;
    uint32_t meshEntries = 0;
    uint32_t materialEntries = 0;
    uint32_t textureEntries = 0;
    uint64_t releasedMeshes = 0;
    uint64_t releasedMeshBytes = 0;
    uint64_t contentsEvicted = 0;
    double unloadDelaySeconds = 0.0;
    double sinceOpenMs = 0.0;
    bool idle = true;
};

// Residency as the main loop keeps it (the `scenethread` record's residency fields).
struct SceneFlowResidency {
    bool resident = false;
    bool everResident = false;
    bool renderThreadDrawn = false;
    bool scriptHeld = false;
    uint64_t scriptFrame = 0;
    uint64_t loadingFrames = 0;
    double longestLoadingFrameMs = 0.0;
    uint64_t lastResidentFrame = 0;
    double lastResidentMs = 0.0;
    uint64_t lastLoadFrames = 0;
    double lastLoadLongestMs = 0.0;
};

// Persistent UI state of the Scene Flow window, and what it asks for: the application sets the data each frame while the window is open,
// and applies and clears the requests.
struct SceneFlowWindowState {
    AssetClientStats assets;
    SceneFlowThread thread;
    SceneGpuStats gpu;
    SceneFlowResidency residency;
    int uploadBudgetMiB = 64;
    int geometryLimitMiB = 0;
    // The live values the fields last took: a change from anywhere (RPC, script) is shown in them.
    uint64_t shownBudget = 0;
    uint64_t shownLimit = 0;
    std::optional<uint64_t> uploadBudgetRequested;  // bytes
    std::optional<uint64_t> geometryLimitRequested; // bytes, 0 for the device's limit
};

// How a scene gets from the asset server onto the screen, stage by stage in the order data flows through them: the asset client's requests,
// the scene thread (compose, extract, what it holds), the change lists to the render thread, the GPU scene (tables, upload queue, geometry
// pool) and residency. Each stage says what it is and where its data goes next. The upload budget and the geometry limit are set here.
auto drawSceneFlowWindow(bool& show, SceneFlowWindowState& state) -> void;
