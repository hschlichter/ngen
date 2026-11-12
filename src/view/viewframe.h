#pragma once

#include "assetgoto.h"
#include "camera.h"
#include "cameraflight.h"
#include "capture.h"
#include "culling.h"
#include "debugdraw.h"
#include "drawlists.h"
#include "ground.h"
#include "rendersnapshot.h"
#include "shadowcascades.h"
#include "viewpicking.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class AssetClient;
class Renderer;
class RenderThread;
class ViewScene;

// The cameras a frame uses, each the main camera unless a tool takes it over (the debug camera renders, and may take the controls, LOD and
// streaming, culling and the shadow cascades). Set each frame after the commands, before the input.
struct FrameCameras {
    Camera* view = nullptr;          // rendered from
    Camera* controlled = nullptr;    // moved by the keys and the mouse
    const Camera* lod = nullptr;     // LOD and streaming measure from it
    const Camera* cull = nullptr;    // GPU culling's frustum
    const Camera* shadows = nullptr; // the shadow cascades are fitted to it
};

// Where the view is in loading its scene, read by the `scenethread` record and the Scene window. Resident: the scene thread has nothing left
// to do and the render thread has drawn every change with nothing left to upload.
struct ViewResidency {
    bool resident = false;
    bool everResident = false;
    // The load in progress: main-loop frames while not resident, and the longest of them.
    uint64_t loadingFrames = 0;
    double longestLoadingFrameMs = 0.0;
    // The last time it became resident: the frame, the time since start, and that load's frames and longest frame.
    uint64_t lastResidentFrame = 0;
    double lastResidentMs = 0.0;
    uint64_t lastLoadFrames = 0;
    double lastLoadLongestMs = 0.0;
};

// The captures the render thread takes this frame for whoever asks (picking, the dumps, the UI's windows), and what came back. Each reader
// adds its watches, then takes its results; a capture result belongs to the first that takes it. The renderer's other readbacks are the
// render tools' (src/rendertools).
struct FrameReadbacks {
    std::vector<CaptureWatch> watches;
    std::vector<CaptureResult> captures;
};

// One frame of ngen-view's engine (src/view/README.md, "The frame"): the main camera and its modes, the selection, the scene's inputs to
// the render thread, residency, culling, the shadow cascades and the snapshot. The main loop calls the steps in order, with the commands and
// the tools' and the UI's hooks (viewhooks.h) between them; no tool or UI code runs here.
class ViewFrame {
public:
    ViewFrame(ViewScene& scene, Renderer& renderer, RenderThread& renderThread, AssetClient& assets);

    // The scene: what the asset server said changed at its source (shaders reload, the scene's assets fetched again), then the scene as the
    // scene thread last published it. Until the scene is first resident the camera follows it.
    auto updateScene() -> void;
    // The frame's time, the loading frames, the flight. A camera the commands moved since the scene update ends the following.
    auto advanceTime(uint64_t nowNs) -> void;
    // Every camera the main camera, before the hooks take any over.
    auto resetCameras() -> void;
    // The framebuffer's size: the projection and the aspect the LOD heuristic measures with.
    auto setViewport(int width, int height) -> void;
    // What the scene and the render thread take from the frame: the LOD camera when it moved, the purposes shown, the upload limits, and
    // the scene's changes since the last frame.
    auto sendSceneInputs() -> void;
    // Whether the scene is resident now; the `SceneResident` event when it becomes so, and the framing the first time.
    auto updateResidency() -> void;

    // The controls, from the window layer's input.
    // Mouse look: turns the controlled camera in place, or around the pivot while orbiting.
    auto look(float dx, float dy) -> void;
    // The wheel: the fly speed, or toward the pivot while orbiting.
    auto wheel(float steps) -> void;
    // Orbiting starts around the surface under a framebuffer pixel: the point at the last pivot's distance along the cursor ray at once,
    // the depth there when the pick comes back.
    auto startOrbit(glm::uvec2 pixel) -> void;
    // Selects the instance under a framebuffer pixel, read back from the GPU a few frames later.
    auto pick(glm::uvec2 pixel) -> void;
    auto deselect() -> void;
    auto selectParent() -> void;
    auto frameSelected() -> void;
    // Frames every draw of the scene.
    auto frameScene() -> void;
    // What the following frames: the frame target once something of it is drawn, else the whole scene.
    auto frameArrived() -> void;
    // The keys as a camera move, after the input: orbiting ends when `orbitHeld` is released; following the ground sets the eye's height.
    auto moveCamera(const CameraMove& move, bool orbitHeld) -> void;

    // GPU culling's frustum for this frame, and the latest readback.
    auto cull() -> void;
    // The render-LOD pops of a readback not counted yet; after the render-LOD records it answers are given.
    auto countLodPops() -> void;
    // The shadow cascades, fitted to the shadow camera and the scene's bounds; the frozen frustum's corners.
    auto fitCascades() -> void;
    // The engine's own debug lines, after the hooks': the camera modes'.
    auto drawCameraModes() -> void;
    // The frame's numbers as profiler plots (src/profile/README.md): the renderer's stats and commands, culling, the scene thread, the asset
    // client, GPU usage and memory. Once a frame, last.
    auto plotFrame() -> void;
    // The watches `readbacks` asks for go with the snapshot when they change; the captures that came back are filled in, picking's taken
    // first.
    auto readBack() -> void;
    // The frame as the render thread draws it; the hooks add the tools' and the UI's parts.
    auto buildSnapshot() -> RenderSnapshot;

    ViewScene& scene;
    Renderer& renderer;
    RenderThread& renderThread;
    AssetClient& assets;

    uint64_t frameCounter = 0;
    uint64_t snapshotsTaken = 0; // scene snapshots taken; the tools rebuild their lists once per snapshot
    float dt = 0.0f;
    bool quit = false;

    // The main camera: everything that places the camera moves it; the cameras the frame uses.
    Camera cam;
    FrameCameras cameras;
    // Until the scene is first resident the camera follows it, framing what has arrived, unless it is moved by hand.
    bool autoFrame = true;
    // The camera the view starts at (`--camera=/path`): a camera the scene authors, placed as soon as the scene names it, before the scene is
    // first resident, so what loads first is what it sees. Empty once placed, or dropped when the camera was moved by hand first.
    std::string startCamera;
    // What the following frames until the scene is first resident (`--camera-frame=/prim`): what is drawn at and under a prim; empty for
    // the whole scene.
    std::string frameTarget;
    CameraModesState cameraModes;
    CameraFlight flight;
    AssetGoToState goTo;

    // The selected prim, by full path (instances expanded); empty for none. A click picks it on the GPU.
    std::string selectedPath;
    ViewPicking picking;

    ViewResidency residency;

    int width = 0;
    int height = 0;
    glm::mat4 proj = glm::mat4(1.0f);
    float viewAspect = 1.0f;           // the view's width over height, as the LOD heuristic and the GPU's pick measure it
    glm::vec2 mouse = glm::vec2(0.0f); // the cursor in window points, for the gizmos

    CullState cullState;
    glm::mat4 cullViewProj = glm::mat4(1.0f);
    CullResult latestCull;      // GPU culling readback, a few frames old
    uint64_t lodPops = 0;       // render-LOD pops (a drawn level changed without a fade) counted over the readbacks seen
    bool readLodStates = false; // read every render-LOD group's state back with the culling readback

    std::array<ShadowCascade, maxShadowCascades> cascades;
    uint32_t cascadeCount = 0;
    std::optional<std::array<glm::vec3, 8>> frozenCorners; // the frozen frustum, drawn out to the shadow distance

    // Everything drawn, as of the last cascade fit: the cascades are fitted to it and the renderer is handed it.
    AABB sceneWorldBounds = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
    // The scene's bounds by chunk of draws, for the cascades and the ground test, read again only for the chunks that changed.
    SceneBoundsCache sceneBounds;
    DebugDraw debugDraw;
    FrameReadbacks readbacks;

private:
    // The meshes the draws use, for the ground test: kept from the scene's changes as they pass to the renderer.
    GroundMeshes groundMeshes;
    // The main camera's pose after the scene update, to see whether the commands moved it.
    glm::vec3 poseBeforeCommands = glm::vec3(0.0f);
    float yawBeforeCommands = 0.0f;
    float pitchBeforeCommands = 0.0f;
    uint64_t lastTicks = 0;
    // The camera last posted to the scene thread for the LOD heuristic.
    glm::vec3 postedCameraPosition;
    int postedCameraWidth = 0;
    int postedCameraHeight = 0;
    uint64_t uploadBudgetChanges = 0;
    uint64_t geometryLimitChanges = 0;
    // The profiler's values, handed to the collector on a change; UINT64_MAX applies them on the first frame.
    uint64_t profileHistoryChanges = UINT64_MAX;
    // plotFrame: what it plotted last, so per-frame plots count what is new; the heap plots' ids, one set per heap.
    uint64_t plottedUploadFrame = 0;
    uint64_t plottedAssetBytes = 0;
    uint64_t plottedCullFrame = 0;
    struct HeapPlots {
        uint32_t usage = 0;
        uint32_t budget = 0;
        uint32_t allocated = 0;
    };
    std::vector<HeapPlots> heapPlots;
    uint64_t profileGpuDetailChanges = UINT64_MAX;
    uint64_t lodPopsFrame = 0;
    std::vector<CaptureWatch> sentWatches;
    bool watchesChanged = false; // since the last snapshot

    // The start camera, placed once the scene names it, unless the camera was moved first.
    auto placeStartCamera() -> void;
};
