#include "assetclient.h"
#include "assetid.h"
#include "camera.h"
#include "culling.h"
#include "debugdraw.h"
#include "imguibackendvulkan.h"
#include "jobsystem.h"
#include "profile.h"
#include "renderdebugjson.h"
#include "renderdoccapture.h"
#include "renderer.h"
#include "rendersnapshot.h"
#include "renderthread.h"
#include "renderworld.h"
#include "rhidevicevulkan.h"
#include "rotategizmo.h"
#include "rpcendpoint.h"
#include "rpcregistry.h"
#include "scalegizmo.h"
#include "sessionscript.h"
#include "shadowcascades.h"
#include "statusbar.h"
#include "trace.h"
#include "translategizmo.h"
#include "viewcommands.h"
#include "viewdumps.h"
#include "viewpicking.h"
#include "viewscene.h"
#include "viewui.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

// Build the RhiWindow contract from an SDL window. This is the only place where the
// window layer (SDL) and the graphics backend meet; the RHI itself never sees SDL.
static auto makeRhiWindowSdl(SDL_Window* window) -> RhiWindow {
    RhiWindow rhiWindow;

    uint32_t extensionCount = 0;
    const auto* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    rhiWindow.instanceExtensions.assign(extensions, extensions + extensionCount);

    rhiWindow.createSurface = [window](void* nativeInstance, void** nativeSurface) -> bool {
        auto instance = (VkInstance) nativeInstance;
        auto* surface = (VkSurfaceKHR*) nativeSurface;
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, surface)) {
            TRACE_ERROR("Engine", "SurfaceCreateFailed", "window").text(std::format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError()));
            return false;
        }
        return true;
    };

    return rhiWindow;
}

namespace {

// Gathers the status bar's numbers each frame: timings from the profiler's frame history, memory from the process and
// the device (sampled four times a second), asset counts from the asset client.
class StatusBarGatherer {
public:
    auto gather(const RhiDevice& device, const AssetClient& assets, const CullResult& cull) -> StatusBarData {
        data.trianglesDrawn = cull.cameraTriangles;
        data.trianglesScene = cull.sceneTriangles;
        profile::frameHistory(frames);
        // Average over the last second of frames, newest first.
        double cpuSum = 0.0;
        double gpuSum = 0.0;
        size_t count = 0;
        size_t gpuCount = 0;
        for (auto it = frames.rbegin(); it != frames.rend() && cpuSum < 1000.0; ++it) {
            cpuSum += it->cpuMs;
            count++;
            if (it->gpuMs > 0.0) {
                gpuSum += it->gpuMs;
                gpuCount++;
            }
        }
        if (count > 0 && cpuSum > 0.0) {
            data.frameMs = cpuSum / (double) count;
            data.fps = 1000.0 / data.frameMs;
        }
        data.gpuMs = gpuCount > 0 ? gpuSum / (double) gpuCount : 0.0;

        auto now = profile::now();
        if (now - lastMemorySampleNs >= 250'000'000ull || lastMemorySampleNs == 0) {
            lastMemorySampleNs = now;
            data.cpuMemoryBytes = residentBytes();
            device.memoryHeaps(heaps);
            data.gpuMemoryBytes = 0;
            for (const auto& heap : heaps) {
                data.gpuMemoryBytes += heap.allocated;
            }
        }

        auto stats = assets.stats();
        data.assetsConnected = true;
        data.assetsRequested = stats.requested;
        data.assetsPacked = stats.packed;
        data.assetsCached = stats.cached;
        data.assetsFailed = stats.failed;
        data.assetsInFlight = stats.inFlight();
        data.assetBytes = stats.bytes;
        return data;
    }

private:
    // The process's resident set, from /proc/self/statm (pages).
    static auto residentBytes() -> uint64_t {
        std::ifstream statm("/proc/self/statm");
        uint64_t size = 0;
        uint64_t resident = 0;
        statm >> size >> resident;
        return resident * (uint64_t) sysconf(_SC_PAGESIZE);
    }

    std::vector<profile::FrameStats> frames;
    std::vector<RhiMemoryHeapInfo> heaps;
    uint64_t lastMemorySampleNs = 0;
    StatusBarData data;
};

} // namespace

auto main(int argc, char* argv[]) -> int {
    // Flags are parsed first; the rest of argv (the scene) is collected into `positional`.
    std::vector<const char*> positional;
    bool enableValidation = false;
    bool forceRenderDebug = false;
    bool loadRenderDoc = false; // --renderdoc: load librenderdoc.so before the device so frames can be captured // --render-debug: keep the render debug snapshot and draw log on without the window
    bool failOnValidation = false;
    bool screenshotsShowUi = false;
    uint64_t maxFrames = 0;
    // --geometry-limit=<MiB>: the most each geometry pool buffer takes, below the device's limit; meshes past it wait, undrawn.
    uint64_t geometryLimitBytes = 0;
    bool rpcEnabled = true; // --no-rpc: no endpoint, no discovery file
    // --no-wait-resident: script frames (and --frames, --screenshot) count from start-up instead of from the scene first being resident.
    bool waitForResident = true;
    SessionScript session;
    std::string sessionError;
    auto flagValue = [](std::string_view arg, std::string_view flag) -> std::string {
        return std::string(arg.substr(flag.size()));
    };
    positional.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--renderdoc") {
            loadRenderDoc = true;
        } else if (arg == "--validation") {
            enableValidation = true;
        } else if (arg == "--no-rpc") {
            rpcEnabled = false;
        } else if (arg == "--render-debug") {
            forceRenderDebug = true;
        } else if (arg == "--show-ui") {
            screenshotsShowUi = true;
        } else if (arg == "--no-wait-resident") {
            waitForResident = false;
        } else if (arg == "--fail-on-validation") {
            failOnValidation = true;
            enableValidation = true;
        } else if (arg.starts_with("--geometry-limit=")) {
            geometryLimitBytes = std::stoull(flagValue(arg, "--geometry-limit=")) * 1024 * 1024;
        } else if (arg.starts_with("--frames=")) {
            maxFrames = std::stoull(flagValue(arg, "--frames="));
        } else if (arg.starts_with("--camera=")) {
            // A pose, or the path of a camera the scene authors.
            auto value = flagValue(arg, "--camera=");
            session.add(0, value.starts_with("/") ? "camera-use" : "camera", value);
        } else if (arg.starts_with("--camera-frame=")) {
            session.add(0, "camera-frame", flagValue(arg, "--camera-frame="));
        } else if (arg.starts_with("--select=")) {
            session.add(0, "select", flagValue(arg, "--select="));
        } else if (arg.starts_with("--view=")) {
            session.add(0, "view", flagValue(arg, "--view="));
        } else if (arg.starts_with("--overlay=")) {
            session.add(0, "overlay", flagValue(arg, "--overlay="));
        } else if (arg.starts_with("--screenshot=")) {
            // Taken on the last frame of --frames, or frame 3 when unbounded; resolved below.
            session.add(UINT64_MAX, "screenshot", flagValue(arg, "--screenshot="));
        } else if (arg.starts_with("--script=")) {
            if (!session.loadFile(flagValue(arg, "--script=").c_str(), sessionError)) {
                std::println(stderr, "--script: {}", sessionError);
                return 1;
            }
        } else {
            positional.push_back(argv[i]);
        }
    }
    {
        // --screenshot without a frame: last frame of --frames, else frame 3 (after the first
        // frames in flight have settled).
        std::vector<SessionCommand> deferred;
        session.takeDue(UINT64_MAX, deferred);
        std::vector<SessionCommand> keep;
        for (auto& c : deferred) {
            if (c.frame == UINT64_MAX) {
                c.frame = maxFrames > 0 ? maxFrames : 3;
            }
            keep.push_back(std::move(c));
        }
        for (auto& c : keep) {
            session.add(c.frame, c.verb, c.args);
        }
    }
    TRACE_EVENT("Engine", "ProcessStarted", "view")
        .text(std::format("ngen-view started, pid {}", getpid()))
        .field("pid", (int64_t) getpid())
        .field("scene", positional.size() >= 2 ? positional[1] : "");
    if (!session.empty()) {
        TRACE_EVENT("Engine", "ScriptLoaded", "view")
            .text(std::format("session script: {} commands, the last at frame {}", session.size(), session.lastFrame()))
            .field("commands", (int64_t) session.size())
            .field("last_frame", (int64_t) session.lastFrame());
    }

    // Every asset comes streamed from this variant's asset server, running in the same working directory (asset ids are
    // relative to it); the view doesn't start without one. The variant is
    // the directory the binary lives in, _out/<platform>/<config>/. The shader request goes out now and is waited for
    // just before the renderer needs the shaders, so packing overlaps loading the scene. The scene's packs, meshes,
    // materials and textures come through the same client, by asset id.
    AssetClient assetClient;
    std::error_code binError;
    auto binDirectory = std::filesystem::canonical("/proc/self/exe", binError).parent_path();
    {
        auto variant = binDirectory.parent_path().filename().string() + "/" + binDirectory.filename().string();
        auto connected = assetClient.connect(variant);
        if (!connected) {
            std::println(stderr, "ngen-view: {}", connected.error());
            return 1;
        }
        TRACE_EVENT("Engine", "AssetServerConnected", connected->label)
            .text(std::format("asset server {} connected: pid {}, port {}", connected->label, connected->pid, connected->port))
            .field("pid", (int64_t) connected->pid)
            .field("port", (int64_t) connected->port);
    }
    std::string sceneId;
    if (positional.size() >= 2) {
        sceneId = assetIdForPath(positional[1]);
        if (sceneId.empty()) {
            std::println(stderr, "ngen-view: {} is not an asset id; give the scene's path relative to the asset server's directory", positional[1]);
            return 1;
        }
    }

    // SDL init and window
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        TRACE_ERROR("Engine", "SdlInitFailed", "window").text(std::format("SDL_Init failed: {}", SDL_GetError()));
        return 1;
    }

    auto windowProps = SDL_CreateProperties();
    SDL_SetStringProperty(windowProps, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "ngen");
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 2560);
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 1440);
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, 1);
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, 1);
    auto* window = SDL_CreateWindowWithProperties(windowProps);
    if (window == nullptr) {
        TRACE_ERROR("Engine", "WindowCreateFailed", "window").text(std::format("SDL_CreateWindowWithProperties failed: {}", SDL_GetError()));
        return 1;
    }
    SDL_DestroyProperties(windowProps);
    // The size the trace last reported; SDL also sends a size event for the size the window starts with.
    int tracedWindowWidth = 0;
    int tracedWindowHeight = 0;
    {
        SDL_GetWindowSizeInPixels(window, &tracedWindowWidth, &tracedWindowHeight);
        const char* driver = SDL_GetCurrentVideoDriver();
        TRACE_EVENT("Engine", "WindowCreated", "window")
            .text(std::format("window created: {}x{} pixels, SDL video driver {}", tracedWindowWidth, tracedWindowHeight, driver != nullptr ? driver : "none"))
            .field("width", tracedWindowWidth)
            .field("height", tracedWindowHeight)
            .field("driver", driver != nullptr ? driver : "");
    }

    // Job system
    JobSystem::init();
    TRACE_EVENT("Engine", "JobSystemStarted", "jobs").text(std::format("job system: {} workers", JobSystem::workerCount())).field("workers", (int64_t) JobSystem::workerCount());

    // RHI device. The window layer (SDL) hands the backend what it needs through
    // hooks; the RHI never includes SDL.
    // Failure after this point must stop the workers and SDL explicitly: SDL3 turns
    // SIGTERM into a quit event that nobody would poll, and lingering threads keep the
    // process alive.
    auto failAfterInit = [&] -> int {
        JobSystem::shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    };

    // RenderDoc must hook the graphics API before the device exists.
    RenderDocCapture renderDoc;
    renderDoc.load(loadRenderDoc);

    RhiDeviceVulkan rhiDevice;
    // The device's diagnostics, validation messages among them, go into the trace.
    RhiDeviceOptions deviceOptions = {
        .enableValidation = enableValidation,
        .onMessage =
            [](RhiMessageSeverity severity, std::string_view message) {
                switch (severity) {
                    case RhiMessageSeverity::Info:
                        TRACE_EVENT("Render", "RhiMessage", "rhi").text(message);
                        break;
                    case RhiMessageSeverity::Warning:
                        TRACE_WARNING("Render", "RhiMessage", "rhi").text(message);
                        break;
                    case RhiMessageSeverity::Error:
                        TRACE_ERROR("Render", "RhiMessage", "rhi").text(message);
                        break;
                }
            },
    };
    if (!rhiDevice.init(makeRhiWindowSdl(window), deviceOptions)) {
        return failAfterInit();
    }

    // Renderer
    RhiExtent2D initialExtent = {};
    {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        initialExtent = {.width = (uint32_t) w, .height = (uint32_t) h};
    }
    ImGuiBackendVulkan imguiBackend(window);
    Renderer renderer;
    StatusBarGatherer statusBar;
    auto rendererInitStart = std::chrono::steady_clock::now();
    if (!renderer.init(&rhiDevice, &imguiBackend, &assetClient, initialExtent)) {
        TRACE_ERROR("Render", "RendererInitFailed", "renderer").text("renderer init failed");
        rhiDevice.waitIdle();
        return failAfterInit();
    }
    {
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rendererInitStart).count();
        TRACE_EVENT("Render", "RendererInitialized", "renderer").text(std::format("renderer initialized in {:.0f} ms", ms)).field("ms", ms);
    }
    renderer.setGeometryLimit(geometryLimitBytes);
    renderer.setValidationEnabled(enableValidation);
    renderer.setScreenshotsShowUi(screenshotsShowUi);

    // The passes have registered their pipelines: their shaders go out in one request, and each pipeline is made when its shaders arrive.
    // Nothing waits for them: the frame is the clear colour and the UI until the lit path's pipelines are there.
    renderer.requestShaders();

    // The scene, composed from its packs as they arrive; no scene argument is an empty scene.
    ViewScene scene(assetClient);
    scene.open(sceneId);

    ViewUI viewUI;
    if (positional.size() > 1) {
        viewUI.setBookmarkPath(std::string(positional[1]) + ".cameras.txt");
    }
    viewUI.setAssetDrawCount([&scene](const std::string& assetId) { return scene.drawCount(assetId); });
    auto primPathOf = [&scene](uint32_t prim) -> std::string {
        return scene.primPath(prim);
    };
    // The selected prim, by full path (instances expanded); empty for none. A click picks it on the GPU.
    std::string selectedPath;
    ViewPicking picking;
    AssetGoToState assetGoTo;

    // Render thread
    RenderThread renderThread;
    renderThread.start(&renderer);

    DebugDraw debugDraw;

    // Camera — worldUp tracks the scene's up axis so the controls stay
    // consistent whether we're looking at a Y-up or Z-up asset.
    auto cam = Camera{
        .position = glm::vec3(2.0f, 1.5f, 2.0f),
        .worldUp = scene.worldUp(),
        .yaw = -135.0f,
        .pitch = -20.0f,
        .speed = 5.0f,
        .mouseSensitivity = 0.15f,
    };
    auto mouseCapture = false;

    renderer.initGizmos(&cam);
    TranslateGizmo translateGizmo;
    RotateGizmo rotateGizmo;
    ScaleGizmo scaleGizmo;
    // Where the gizmo of the selection sits: its pivot, or its bounds' centre when the pivot lies outside what it draws.
    auto gizmoAnchorOf = [&](const ViewScene::PrimTransform& xf) -> glm::vec3 {
        auto anchor = glm::vec3(xf.world[3]);
        auto bb = scene.bounds(selectedPath);
        if (bb.valid() && !bb.contains(anchor)) {
            anchor = (bb.min + bb.max) * 0.5f;
        }
        return anchor;
    };
    auto frameSelected = [&] -> void {
        auto bb = scene.bounds(selectedPath);
        if (!selectedPath.empty() && bb.valid()) {
            cam.frame(bb, glm::radians(45.0f));
        }
    };
    auto selectParent = [&] -> void {
        auto slash = selectedPath.rfind('/');
        selectedPath = slash == 0 || slash == std::string::npos ? std::string() : selectedPath.substr(0, slash);
    };

    // Union the world bounds of all mesh instances and frame the camera on them.
    // Used at startup so the initial view works for any up-axis and asset scale.
    auto frameSceneView = [&] -> void {
        AABB sceneBounds = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
        for (const auto& inst : scene.renderWorld().meshInstances) {
            if (inst.alive && inst.worldBounds.valid()) {
                sceneBounds.min = glm::min(sceneBounds.min, inst.worldBounds.min);
                sceneBounds.max = glm::max(sceneBounds.max, inst.worldBounds.max);
            }
        }
        if (sceneBounds.valid()) {
            cam.frame(sceneBounds, glm::radians(45.0f));
        }
    };
    // Until the scene is first resident the camera follows it, framing what has arrived, unless it is moved by hand.
    bool autoFrame = true;
    // Answers waiting for the scene to be resident (view.waitResident, view.lod.set, view.repack, view.purposes).
    std::vector<ResidentWaiter> residentWaiters;
    // Script frames count only while nothing waits for residency, so a script sees the scene as it is once loaded; the longest main-loop
    // frame while the scene isn't resident says how responsive the view stayed while it loaded.
    ViewResidency residency;
    // The camera last posted to the scene thread for the LOD heuristic.
    glm::vec3 postedCameraPosition(std::numeric_limits<float>::quiet_NaN());
    int postedCameraWidth = 0;
    int postedCameraHeight = 0;

    // Main loop
    profile::registerThread("Main");
    auto lastTicks = SDL_GetTicksNS();
    auto quit = false;
    uint64_t frameCounter = 0;
    CullState cullState;
    CullResult latestCull; // GPU culling readback, a few frames old
    std::vector<SessionCommand> dueCommands;

    // Dumps and captures that finish frames later, for script verbs and RPC calls alike.
    ViewDumps dumps(scene.renderWorld(), primPathOf, latestCull, rhiDevice.limits().pipelineStatistics);

    // Every command is an RPC method; script lines and live calls run the same code.
    RpcRegistry registry;
    registerRpcBuiltins(registry, "view");
    ViewContext viewContext = {
        .cam = cam,
        .scene = scene,
        .ui = viewUI,
        .selectedPath = selectedPath,
        .picking = picking,
        .goTo = assetGoTo,
        .renderer = renderer,
        .renderThread = renderThread,
        .renderDoc = renderDoc,
        .dumps = dumps,
        .latestCull = latestCull,
        .assets = assetClient,
        .frameSceneView = frameSceneView,
        .quit = quit,
        .frameCounter = frameCounter,
        .residentWaiters = residentWaiters,
        .residency = residency,
        .sceneLabel = positional.size() >= 2 ? std::string(positional[1]) : std::string("(new scene)"),
    };
    registerViewMethods(registry, viewContext);
    RpcRecords records;
    registerViewRecords(records, registry, viewContext);
    registerRpcRecords(registry, records);
    // The endpoint is tooling: debug and release only (NGEN_INTROSPECTION), off with --no-rpc.
    RpcEndpoint endpoint;
#ifdef NGEN_INTROSPECTION
    if (rpcEnabled) {
        endpoint.start(&registry, "view", viewContext.sceneLabel);
    }
#else
    (void) rpcEnabled;
#endif

    TRACE_EVENT("Engine", "MainLoopStarted", "view").text(std::format("main loop started, {:.0f} ms after start", trace::msSinceStart())).field("since_start_ms", trace::msSinceStart());
    while (!quit && !viewUI.wantsQuit()) {
        PROFILE_FRAME_MARK();
        frameCounter++;
        // The scene as the scene thread last published it. Until it is first resident the camera follows it.
        if (scene.update()) {
            cam.worldUp = scene.worldUp();
            if (autoFrame) {
                frameSceneView();
            }
            // The cameras the scene authors, as poses for the Camera window.
            auto& sceneCameras = viewUI.cameraWindow().sceneCameras;
            sceneCameras.clear();
            for (const auto& authored : scene.cameras()) {
                Camera placed = cam;
                placed.place(authored.worldTransform);
                sceneCameras.push_back({.name = authored.path, .pose = {placed.position.x, placed.position.y, placed.position.z, placed.yaw, placed.pitch}});
            }
            // The LOD roots, for the LOD window.
            auto& lodRows = viewUI.lodWindow().rows;
            lodRows.clear();
            for (const auto& root : scene.lodRoots()) {
                std::string heuristic;
                if (!root.heuristicType.empty()) {
                    std::string thresholds;
                    for (float threshold : root.thresholds) {
                        thresholds += std::format("{}{:g}", thresholds.empty() ? "" : ", ", threshold);
                    }
                    heuristic = std::format("{} [{}]", root.heuristicType, thresholds);
                }
                const auto& choices = scene.lodChoices();
                auto choice = choices.find(root.path);
                std::string waiting;
                if (root.waiting) {
                    waiting = std::format("Waits for {} meshes ({} asked for) and {} contents composed without every pack.",
                                          root.waiting->meshesMissing,
                                          root.waiting->meshesInFlight,
                                          root.waiting->contentsIncomplete);
                }
                lodRows.push_back({
                    .path = root.path,
                    .levels = root.levels,
                    .levelPayloads = root.levelPayloads,
                    .heuristic = heuristic,
                    .defaultIndex = root.defaultIndex,
                    .wanted = root.wanted,
                    .drawn = root.drawn ? (int32_t) *root.drawn : -1,
                    .loaded = root.loaded,
                    .setByHand = root.setByHand,
                    .screenSize = choice != choices.end() ? choice->second.screenSize : -1.0f,
                    .why = choice != choices.end() ? choice->second.reason : std::string("not evaluated yet"),
                    .waiting = waiting,
                });
            }
        }
        residency.scriptHeld = (waitForResident && !residency.everResident) || !residentWaiters.empty();
        dueCommands.clear();
        if (!residency.scriptHeld) {
            residency.scriptFrame++;
            session.takeDue(residency.scriptFrame, dueCommands);
        }
        // A script or an RPC call that moves the camera ends the following, as a hand on the controls does.
        auto camPositionBefore = cam.position;
        auto camYawBefore = cam.yaw;
        auto camPitchBefore = cam.pitch;
        for (const auto& c : dueCommands) {
            runViewScriptCommand(registry, c);
        }
        endpoint.drain();
        if (cam.position != camPositionBefore || cam.yaw != camYawBefore || cam.pitch != camPitchBefore) {
            autoFrame = false;
        }
        if (maxFrames > 0 && residency.scriptFrame > maxFrames) {
            quit = true;
        }
        auto nowTicks = SDL_GetTicksNS();
        auto dt = (float) (nowTicks - lastTicks) / 1.0e9f;
        lastTicks = nowTicks;
        if (!residency.resident && frameCounter > 1) {
            residency.longestLoadingFrameMs = std::max(residency.longestLoadingFrameMs, (double) dt * 1000.0);
            residency.loadingFrames++;
        }

        int winW = 0;
        int winH = 0;
        SDL_GetWindowSizeInPixels(window, &winW, &winH);
        RhiExtent2D winExtent = {.width = (uint32_t) winW, .height = (uint32_t) winH};
        auto proj = cameraProjection((float) winW / (float) winH);
        // The camera for the LOD heuristic, when it moved: once it has been placed (by hand, a script, or the framing when the scene was
        // first resident), not while it follows what has arrived.
        bool cameraPlaced = residency.everResident || !autoFrame;
        if (cameraPlaced && (cam.position != postedCameraPosition || winW != postedCameraWidth || winH != postedCameraHeight)) {
            postedCameraPosition = cam.position;
            postedCameraWidth = winW;
            postedCameraHeight = winH;
            scene.setCamera(cam.position, glm::radians(cameraFovDegrees), (float) winW / (float) std::max(1, winH));
        }

        // The purposes the View menu or a script turned on or off: the scene loads and draws them, and moves its revision on.
        if (scene.purposeShown(Purpose::Guide) != viewUI.showGuides()) {
            scene.setPurposeShown(Purpose::Guide, viewUI.showGuides());
        }
        if (scene.purposeShown(Purpose::Proxy) != viewUI.showProxies()) {
            scene.setPurposeShown(Purpose::Proxy, viewUI.showProxies());
        }
        // What changed in the scene since the last frame: the render thread applies it, uploads go out a budget per frame.
        if (auto changes = scene.takeChanges(); !changes.empty()) {
            renderThread.submitSceneChanges(std::move(changes));
        }
        // Resident: the scene thread has nothing left to do, and the render thread has drawn every change with nothing left to upload.
        bool resident = scene.idle() && renderThread.sceneDrawn();
        if (resident && !residency.resident) {
            if (!residency.everResident) {
                residency.everResident = true;
                if (autoFrame) {
                    frameSceneView();
                }
                autoFrame = false;
            }
            if (!residentWaiters.empty() || frameCounter > 0) {
                auto stats = assetClient.stats();
                TRACE_EVENT("Scene", "SceneResident", scene.sceneId().empty() ? "(empty scene)" : scene.sceneId())
                    .text(std::format("scene resident at frame {}, {:.0f} ms after start: {} draws, {} assets received; longest of the {} frames before, {:.1f} ms",
                                      frameCounter,
                                      trace::msSinceStart(),
                                      std::ranges::count_if(scene.renderWorld().meshInstances, [](const RenderMeshInstance& instance) {
                                          return instance.alive;
                                      }),
                                      stats.received,
                                      residency.loadingFrames,
                                      residency.longestLoadingFrameMs))
                    .field("frame", (int64_t) frameCounter)
                    .field("since_start_ms", trace::msSinceStart())
                    .field("assets", (int64_t) stats.received)
                    .field("longest_frame_ms", residency.longestLoadingFrameMs);
            }
            residency.lastResidentFrame = frameCounter;
            residency.lastResidentMs = trace::msSinceStart();
            residency.lastLoadFrames = residency.loadingFrames;
            residency.lastLoadLongestMs = residency.longestLoadingFrameMs;
            residency.longestLoadingFrameMs = 0.0;
            residency.loadingFrames = 0;
        }
        residency.resident = resident;
        if (resident) {
            for (auto& waiter : residentWaiters) {
                waiter.responder.respond(waiter.result);
            }
            residentWaiters.clear();
        }

        static const uint32_t pollZoneId = profile::registerName("PollEvents");
        profile::beginZone(pollZoneId);
        uint64_t polledEvents = 0;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            polledEvents++;
            bool uiCaptured = false;
            {
                PROFILE_ZONE("ImGuiEvent");
                uiCaptured = imguiBackend.processEvent(&ev);
            }

            if (ev.type == SDL_EVENT_QUIT) {
                TRACE_EVENT("Engine", "QuitRequested", "view").text("quitting");
                quit = true;
            }
            if (ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                bool sizeChanged = ev.window.data1 != tracedWindowWidth || ev.window.data2 != tracedWindowHeight;
                if (sizeChanged) {
                    tracedWindowWidth = ev.window.data1;
                    tracedWindowHeight = ev.window.data2;
                    TRACE_EVENT("Engine", "WindowResized", "window")
                        .text(std::format("window resized to {}x{} pixels", ev.window.data1, ev.window.data2))
                        .field("width", ev.window.data1)
                        .field("height", ev.window.data2);
                }
            }

            if (uiCaptured) {
                continue;
            }

            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F12 && !ev.key.repeat) {
                viewUI.requestScreenshot();
                continue;
            }
            bool plainKey = ev.type == SDL_EVENT_KEY_DOWN && (ev.key.mod & SDL_KMOD_CTRL) == 0 && !ev.key.repeat;
            // Z deselects, R selects the parent, F frames the selection, 1-4 pick the tool.
            if (plainKey && ev.key.key == SDLK_Z) {
                selectedPath.clear();
                continue;
            }
            if (plainKey && ev.key.key == SDLK_R) {
                selectParent();
                continue;
            }
            if (plainKey && ev.key.key == SDLK_F) {
                frameSelected();
                continue;
            }
            if (plainKey) {
                switch (ev.key.key) {
                    case SDLK_1:
                        viewUI.setActiveTool(ViewTool::Select);
                        continue;
                    case SDLK_2:
                        viewUI.setActiveTool(ViewTool::Translate);
                        continue;
                    case SDLK_3:
                        viewUI.setActiveTool(ViewTool::Rotate);
                        continue;
                    case SDLK_4:
                        viewUI.setActiveTool(ViewTool::Scale);
                        continue;
                    default:
                        break;
                }
            }

            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_RIGHT) {
                mouseCapture = true;
            }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_RIGHT) {
                mouseCapture = false;
            }
            if (ev.type == SDL_EVENT_MOUSE_MOTION && mouseCapture) {
                cam.handleMouseMotion(ev.motion.xrel, ev.motion.yrel);
                autoFrame = false;
            }
            // The wheel changes the fly speed.
            if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                cam.scaleSpeed(ev.wheel.y);
                continue;
            }

            // A gizmo drag in progress: the new local transform is the view's own opinion on the prim, until the view closes.
            bool dragging = translateGizmo.isDragging() || rotateGizmo.isDragging() || scaleGizmo.isDragging();
            auto dragTo = [&](float x, float y) -> std::optional<Transform> {
                if (translateGizmo.isDragging()) {
                    return translateGizmo.dragUpdate(x, y, winExtent, cam.viewMatrix(), proj);
                }
                if (rotateGizmo.isDragging()) {
                    return rotateGizmo.dragUpdate(x, y, winExtent, cam.viewMatrix(), proj);
                }
                return scaleGizmo.dragUpdate(x, y, winExtent, cam.viewMatrix(), proj);
            };
            if (ev.type == SDL_EVENT_MOUSE_MOTION && dragging) {
                if (auto local = dragTo(ev.motion.x, ev.motion.y)) {
                    scene.setLocalTransform(selectedPath, local->toMat4());
                }
                continue;
            }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT && dragging) {
                if (auto local = dragTo(ev.button.x, ev.button.y)) {
                    scene.setLocalTransform(selectedPath, local->toMat4());
                }
                translateGizmo.release();
                rotateGizmo.release();
                scaleGizmo.release();
                continue;
            }

            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
                float mx = ev.button.x;
                float my = ev.button.y;
                // The axis gizmo turns the camera to an axis.
                if (renderer.gizmoHitTest(mx, my, winExtent)) {
                    continue;
                }
                if (auto xf = scene.transform(selectedPath); xf && xf->xformable && !selectedPath.empty()) {
                    auto tool = viewUI.activeTool();
                    auto local = decomposeTransform(xf->local);
                    auto anchor = gizmoAnchorOf(*xf);
                    bool grabbed = false;
                    if (tool == ViewTool::Translate) {
                        grabbed = translateGizmo.tryGrab(mx, my, winExtent, cam.viewMatrix(), proj, anchor, local, xf->world);
                    } else if (tool == ViewTool::Rotate) {
                        grabbed = rotateGizmo.tryGrab(mx, my, winExtent, cam.viewMatrix(), proj, anchor, local, xf->world);
                    } else if (tool == ViewTool::Scale) {
                        grabbed = scaleGizmo.tryGrab(mx, my, winExtent, cam.viewMatrix(), proj, anchor, local, xf->world);
                    }
                    if (grabbed) {
                        continue;
                    }
                }
                // Anything else picks: the instance under the cursor, read back from the GPU a few frames later.
                int pointsW = 0;
                int pointsH = 0;
                SDL_GetWindowSize(window, &pointsW, &pointsH);
                auto px = (uint32_t) std::max(0.0f, mx * (float) winW / (float) std::max(pointsW, 1));
                auto py = (uint32_t) std::max(0.0f, my * (float) winH / (float) std::max(pointsH, 1));
                picking.request(px, py, [&scene, &selectedPath](std::optional<uint32_t> instance) {
                    selectedPath = instance ? scene.instancePath(*instance) : std::string();
                });
            }
        }

        profile::zoneValue(polledEvents);
        profile::endZone(); // PollEvents

        const auto* keys = SDL_GetKeyboardState(nullptr);
        auto camBefore = cam.position;
        cam.update(keys, dt);
        if (cam.position != camBefore) {
            autoFrame = false;
        }

        // Culling runs on the GPU: the snapshot carries the frustum
        // to cull against (live, or frozen so the culled set can be inspected from elsewhere),
        // and the results come back a few frames later for the Culling window and overlay.
        cullState.enabled = viewUI.getCullEnabled();
        cullState.frozen = viewUI.getCullFrozen();
        auto cullViewProj = cullState.update(proj * cam.viewMatrix());
        if (auto cull = renderThread.latestCullResult(); cull.has_value()) {
            latestCull = std::move(*cull);
        }
        viewUI.setCullStats(latestCull.instances, latestCull.cameraCulled);
        viewUI.setShadowCullStats(latestCull.cascadeCount, latestCull.cascadeCulled, latestCull.cascadeDrawn);
        viewUI.setCullTriangles(latestCull.cameraTriangles, latestCull.sceneTriangles, latestCull.cascadeTriangles);

        // Shadow cascades: fitted to the live camera and the scene bounds.
        std::array<ShadowCascade, maxShadowCascades> cascades;
        uint32_t cascadeCount = 0;
        {
            PROFILE_ZONE("FitCascades");
            auto sceneBounds = sceneBoundsOf(scene.renderWorld().meshInstances);
            auto lightDir = pickShadowLightDirection(scene.renderWorld().lights, cam.worldUp);
            cascadeCount = fitShadowCascades(viewUI.getShadowSettings(), cam.viewMatrix(), proj, cameraNear, lightDir, cam.worldUp, sceneBounds, cascades);
        }
        std::array<glm::vec3, 8> frozenCorners;
        if (cullState.frozenActive) {
            // The frozen frustum has no far plane: drawn out to the shadow distance.
            frozenCorners = cullState.frozenCorners(cameraDepthAt(viewUI.getShadowSettings().maxDistance));
        }

        // The AABB overlay shows one view's GPU culling result: the camera or a cascade.
        std::vector<uint8_t> overlayVisible(latestCull.viewBits.size());
        auto overlayBit = (uint8_t) (1u << std::clamp(viewUI.cullOverlayView(), 0, (int) maxShadowCascades));
        for (size_t i = 0; i < overlayVisible.size(); i++) {
            overlayVisible[i] = (latestCull.viewBits[i] & overlayBit) != 0 ? 1 : 0;
        }
        auto selectedBounds = scene.bounds(selectedPath);
        viewUI.drawDebug(debugDraw, scene.renderWorld(), cam.position, cam.worldUp, overlayVisible, cullState.frozenActive ? &frozenCorners : nullptr, std::span<const ShadowCascade>(cascades.data(), cascadeCount), selectedPath.empty() ? nullptr : &selectedBounds);

        renderThread.setFrameGraphDebugEnabled(viewUI.wantsFrameGraphDebug() || dumps.wantsFrameGraphDebug());
        auto fgDebugSnap = renderThread.latestFrameGraphDebug();
        dumps.onFrameGraph(fgDebugSnap);
        bool frameDebugPending = dumps.takeFrameDebugRequest();
        if (viewUI.takeFrameDebugRequest()) {
            frameDebugPending = true;
        }
        renderThread.setRenderDebugEnabled(viewUI.wantsRenderDebug() || forceRenderDebug || dumps.wantsRenderDebug());
        auto renderDebugSnap = renderThread.latestRenderDebug();
        dumps.onRenderDebug(renderDebugSnap);
        {
            // Capture watches: the editor's windows plus pending dumps, sent when they change.
            static std::vector<CaptureWatch> sentWatches;
            auto watches = viewUI.captureWatches();
            dumps.appendWatches(watches);
            picking.appendWatches(watches);
            if (watches != sentWatches || frameDebugPending) {
                sentWatches = watches;
                renderThread.setCaptureWatches(watches, frameDebugPending);
            }
            if (auto frameDebug = renderThread.latestFrameDebug(); frameDebug.has_value()) {
                dumps.onFrameDebug(*frameDebug);
                viewUI.onFrameDebugCapture(std::move(*frameDebug));
            }
            for (auto& result : renderThread.takeCaptureResults()) {
                if (picking.onCaptureResult(result)) {
                    continue;
                }
                if (!dumps.onCaptureResult(result)) {
                    viewUI.onCaptureResult(std::move(result));
                }
            }
            dumps.onScreenshots(renderThread.takeScreenshotResults());
        }
        renderThread.setCountersEnabled(viewUI.wantsCounters() || dumps.wantsCounters());
        for (auto& counters : renderThread.takeCounters()) {
            dumps.onCounters(counters);
            viewUI.onCounters(std::move(counters));
        }

        static const uint32_t uiZoneId = profile::registerName("EditorUI");
        profile::beginZone(uiZoneId);
        imguiBackend.beginFrame();
        viewUI.setStatusBar(statusBar.gather(rhiDevice, assetClient, latestCull));
        if (viewUI.wantsAssetLog()) {
            viewUI.setAssetLog(assetClient.log());
        }
        // Repacks the Assets window asked for: blocking, as loading the scene is; the rebuilt scene uploads as any change does.
        viewUI.setHasPrimSelected(!selectedPath.empty());
        if (viewUI.takeFindSelectedRequest()) {
            viewUI.selectAssets(scene.assetsUnder(selectedPath));
        }
        if (auto goTo = viewUI.takeGoToRequest()) {
            auto result = goToAsset(viewContext, goTo->id, goTo->all);
            if (!result.error.empty()) {
                viewUI.setGoToStatus(result.error);
            } else if (goTo->all) {
                viewUI.setGoToStatus(std::format("{}: all {} draws framed", goTo->id, result.count));
            } else {
                viewUI.setGoToStatus(std::format("{}: draw {} of {}, {}", goTo->id, result.index + 1, result.count, result.prim));
            }
        }
        if (auto repack = viewUI.takeRepackRequest(); repack.scene) {
            scene.refresh(scene.assetIds(), true);
        } else if (!repack.ids.empty()) {
            scene.refresh(repack.ids, true);
        }
        // What the asset server said changed at its source: shaders reload on the render thread, the scene's assets are fetched again and
        // the scene rebuilt (blocking, as a repack is).
        if (auto changed = assetClient.takeChanged(); !changed.empty()) {
            renderThread.reloadShaders(changed);
            scene.refresh(changed, false);
        }
        viewUI.draw(primPathOf, !selectedPath.empty(), scene.renderWorld(), cam, std::move(fgDebugSnap), std::move(renderDebugSnap));
        switch (viewUI.takeEditRequest()) {
            case ViewEditRequest::Deselect:
                selectedPath.clear();
                break;
            case ViewEditRequest::SelectParent:
                selectParent();
                break;
            case ViewEditRequest::FrameSelected:
                frameSelected();
                break;
            case ViewEditRequest::None:
                break;
        }
        if (auto inspect = viewUI.takeTextureInspectRequest(); inspect.has_value()) {
            renderThread.setTextureInspect(*inspect);
        }
        if (auto dump = viewUI.takeTextureDumpRequest(); dump.has_value()) {
            renderer.requestTextureDump(dump->texture, dump->level, "texture_" + std::to_string(dump->texture) + "_L" + std::to_string(dump->level) + ".png");
        }
        {
            auto& flags = viewUI.introspection();
            flags.renderDocAvailable = renderDoc.available();
            if (flags.renderDocCaptureRequested) {
                flags.renderDocCaptureRequested = false;
                renderDoc.triggerCapture();
            }
            if (flags.renderDocOpenRequested) {
                flags.renderDocOpenRequested = false;
                if (!renderDoc.openInUi(renderDoc.lastCapture())) {
                    TRACE_WARNING("Engine", "RenderDocOpenFailed", "renderdoc").text("RenderDoc: no capture to open");
                }
            }
            for (const auto& path : renderDoc.pollNewCaptures()) {
                TRACE_EVENT("Engine", "RenderDocCapture", path).text(std::format("RenderDoc capture written: {}", path)).field("frame", (int64_t) frameCounter);
            }
        }
        if (viewUI.takeScreenshotRequest()) {
            renderer.requestScreenshot("screenshot_" + std::to_string(frameCounter) + ".png");
        }
        {
            auto& camWindow = viewUI.cameraWindow();
            if (camWindow.requestFrameScene) {
                camWindow.requestFrameScene = false;
                frameSceneView();
            }
            if (camWindow.requestFrameSelected) {
                camWindow.requestFrameSelected = false;
                frameSelected();
            }
        }
        if (viewUI.wantsEngineMap()) {
            viewUI.setEngineMapStats(engineMapStats(viewContext));
        }
        if (viewUI.wantsSceneFlow()) {
            // The Scene Flow window: the same numbers as the scenethread and gpuscene records.
            auto& flow = viewUI.sceneFlowWindow();
            const auto& t = scene.threadStats();
            auto [posted, applied] = scene.requestCounts();
            flow.assets = assetClient.stats();
            flow.thread = {
                .requestsPosted = posted,
                .requestsApplied = applied,
                .batches = t.batches,
                .composes = t.composes,
                .lastComposeMs = t.lastComposeMs,
                .composeNeeded = t.composeNeeded,
                .prototypesComposed = t.prototypesComposed,
                .levelsComposed = t.levelsComposed,
                .prototypesHeld = t.prototypesHeld,
                .levelsHeld = t.levelsHeld,
                .contentsReached = t.contentsReached,
                .extracts = t.extracts,
                .lastExtractMs = t.lastExtractMs,
                .extractVisitMs = t.extractVisitMs,
                .extractSlotsMs = t.extractSlotsMs,
                .extractReused = t.extractReused,
                .extractWalked = t.extractWalked,
                .extractKept = t.extractKept,
                .extractNeeded = t.extractNeeded,
                .assetsReceived = t.assetsReceived,
                .inFlightLayers = t.inFlightLayers,
                .inFlightMeshes = t.inFlightMeshes,
                .inFlightMaterials = t.inFlightMaterials,
                .inFlightTextures = t.inFlightTextures,
                .inFlightOther = t.inFlightOther,
                .layerPacks = t.layerPacks,
                .meshPacks = t.meshPacks,
                .materialPacks = t.materialPacks,
                .texturePacks = t.texturePacks,
                .failedAssets = t.failedAssets,
                .meshEntries = t.meshEntries,
                .materialEntries = t.materialEntries,
                .textureEntries = t.textureEntries,
                .releasedMeshes = t.releasedMeshes,
                .releasedMeshBytes = t.releasedMeshBytes,
                .contentsEvicted = t.contentsEvicted,
                .unloadDelaySeconds = scene.unloadDelay(),
                .sinceOpenMs = t.sinceOpenMs,
                .idle = scene.idle(),
            };
            flow.gpu = renderThread.latestSceneGpuStats();
            flow.residency = {
                .resident = residency.resident,
                .everResident = residency.everResident,
                .renderThreadDrawn = renderThread.sceneDrawn(),
                .scriptHeld = residency.scriptHeld,
                .scriptFrame = residency.scriptFrame,
                .loadingFrames = residency.loadingFrames,
                .longestLoadingFrameMs = residency.longestLoadingFrameMs,
                .lastResidentFrame = residency.lastResidentFrame,
                .lastResidentMs = residency.lastResidentMs,
                .lastLoadFrames = residency.lastLoadFrames,
                .lastLoadLongestMs = residency.lastLoadLongestMs,
            };
            if (auto bytes = std::exchange(flow.uploadBudgetRequested, std::nullopt)) {
                renderThread.setUploadBudget(*bytes);
            }
            if (auto bytes = std::exchange(flow.geometryLimitRequested, std::nullopt)) {
                renderThread.setGeometryLimit(*bytes);
            }
        }
        {
            // The LOD window: the levels it asked for, and the root it selects or frames; it highlights the root at or above the selection.
            auto& lodWindow = viewUI.lodWindow();
            for (const auto& request : std::exchange(lodWindow.levelRequests, {})) {
                if (request.level) {
                    scene.setLodLevel(request.root, *request.level);
                } else {
                    scene.resetLodLevel(request.root);
                }
            }
            lodWindow.heuristic = scene.lodHeuristic();
            lodWindow.cap = (int) scene.lodCap();
            lodWindow.unloadDelay = (float) scene.unloadDelay();
            if (auto on = std::exchange(lodWindow.heuristicRequested, std::nullopt)) {
                scene.setLodHeuristic(*on);
            }
            if (auto cap = std::exchange(lodWindow.capRequested, std::nullopt)) {
                scene.setLodCap((uint32_t) *cap);
            }
            if (auto delay = std::exchange(lodWindow.unloadDelayRequested, std::nullopt)) {
                scene.setUnloadDelay(*delay);
            }
            if (auto path = std::exchange(lodWindow.selectRequested, std::nullopt)) {
                selectedPath = *path;
            }
            if (auto path = std::exchange(lodWindow.frameRequested, std::nullopt)) {
                auto bb = scene.bounds(*path);
                if (bb.valid()) {
                    cam.frame(bb, glm::radians(45.0f));
                }
            }
            lodWindow.selectedRoot.clear();
            if (!selectedPath.empty()) {
                for (const auto& row : lodWindow.rows) {
                    bool atOrAbove = selectedPath == row.path || selectedPath.starts_with(row.path + "/");
                    if (atOrAbove && row.path.size() > lodWindow.selectedRoot.size()) {
                        lodWindow.selectedRoot = row.path;
                    }
                }
            }
        }
        ImGuiFrameSnapshot imguiSnapshot;
        {
            PROFILE_ZONE("ImGuiRender");
            imguiSnapshot = imguiBackend.endFrame();
        }
        profile::endZone(); // EditorUI

        static const uint32_t snapshotZoneId = profile::registerName("BuildSnapshot");
        profile::beginZone(snapshotZoneId);

        float mouseX = 0;
        float mouseY = 0;
        SDL_GetMouseState(&mouseX, &mouseY);

        // The active tool's gizmo, on the selection when it takes a transform.
        auto selectedXf = selectedPath.empty() ? std::nullopt : scene.transform(selectedPath);
        bool showTransformGizmo = selectedXf && selectedXf->xformable;
        auto gizmoAnchor = showTransformGizmo ? gizmoAnchorOf(*selectedXf) : glm::vec3(0.0f);
        auto activeTool = viewUI.activeTool();
        translateGizmo.update(winExtent, cam.viewMatrix(), proj, cam.position, mouseX, mouseY, showTransformGizmo && activeTool == ViewTool::Translate, gizmoAnchor);
        rotateGizmo.update(winExtent, cam.viewMatrix(), proj, cam.position, mouseX, mouseY, showTransformGizmo && activeTool == ViewTool::Rotate, gizmoAnchor);
        scaleGizmo.update(winExtent, cam.viewMatrix(), proj, cam.position, mouseX, mouseY, showTransformGizmo && activeTool == ViewTool::Scale, gizmoAnchor);

        RenderSnapshot snapshot = {
            .viewMatrix = cam.viewMatrix(),
            .projMatrix = proj,
            .worldUp = cam.worldUp,
            .windowWidth = winW,
            .windowHeight = winH,
            .mouseX = mouseX,
            .mouseY = mouseY,
            .showGizmo = viewUI.getShowGizmo(),
            .gbufferViewMode = static_cast<GBufferView>(viewUI.getGBufferViewMode()),
            .debugView = static_cast<DebugView>(viewUI.getDebugView()),
            .showBufferOverlay = viewUI.getShowBufferOverlay(),
            .showShadowOverlay = viewUI.getShowShadowOverlay(),
            .antiAliasing = viewUI.getAntiAliasing(),
            .depthPrepass = viewUI.getDepthPrepass(),
            .sampler = viewUI.getSamplerSettings(),
            .cullEnabled = cullState.enabled,
            .cullViewProj = cullViewProj,
            .shadowSettings = viewUI.getShadowSettings(),
            .ambient = viewUI.ambientSettings(),
            .cascadeCount = cascadeCount,
            .cascades = cascades,
            .translateGizmoVerts = {translateGizmo.vertices().begin(), translateGizmo.vertices().end()},
            .rotateGizmoVerts = {rotateGizmo.vertices().begin(), rotateGizmo.vertices().end()},
            .scaleGizmoVerts = {scaleGizmo.vertices().begin(), scaleGizmo.vertices().end()},
            .debugData = debugDraw.data(),
            .imguiSnapshot = std::move(imguiSnapshot),
        };

        profile::endZone(); // BuildSnapshot
        {
            PROFILE_ZONE("SubmitSnapshot");
            renderThread.submitSnapshot(std::move(snapshot));
        }
    }

    auto validationErrors = rhiDevice.validationErrorCount();

    renderThread.stop();
    JobSystem::shutdown();
    bool pipelinesFailed = renderer.pipelinesFailed();
    renderer.destroy();
    rhiDevice.destroy();
    SDL_DestroyWindow(window);
    SDL_Quit();

    // The endpoint goes last, so subscribers get the trace up to here: the last events are sent before the
    // connections close, waiting up to a second for a subscriber to take them.
    bool failed = failOnValidation && (validationErrors > 0 || pipelinesFailed);
    if (failOnValidation && validationErrors > 0) {
        TRACE_ERROR("Engine", "ValidationFailed", "view").text(std::format("validation errors: {}", validationErrors)).field("count", (int64_t) validationErrors);
    }
    if (failOnValidation && pipelinesFailed) {
        TRACE_ERROR("Engine", "PipelinesFailed", "view").text("a pipeline failed: its shader didn't pack or the device couldn't create it (the reason is above)");
    }
    TRACE_EVENT("Engine", "ProcessExiting", "view")
        .text(std::format("exiting after {} frames", frameCounter))
        .field("frames", (int64_t) frameCounter)
        .field("validation_errors", (int64_t) validationErrors);
    endpoint.finishTrace(std::chrono::seconds(1));
    endpoint.stop();

    if (failed) {
        return 2;
    }
    return 0;
}
