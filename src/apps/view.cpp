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
    bool rpcEnabled = true; // --no-rpc: no endpoint, no discovery file
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
        } else if (arg == "--fail-on-validation") {
            failOnValidation = true;
            enableValidation = true;
        } else if (arg.starts_with("--frames=")) {
            maxFrames = std::stoull(flagValue(arg, "--frames="));
        } else if (arg.starts_with("--camera=")) {
            session.add(0, "camera", flagValue(arg, "--camera="));
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
    renderer.setValidationEnabled(enableValidation);
    renderer.setScreenshotsShowUi(screenshotsShowUi);

    // The passes have registered their pipelines: their start-up shaders go out in one request now and pack while the scene loads.
    auto stopAfterRenderer = [&] -> int {
        renderer.destroy();
        rhiDevice.destroy();
        return failAfterInit();
    };
    renderer.requestStartupShaders();

    // The scene, composed from its packs; no scene argument is an empty scene.
    ViewScene scene(assetClient);
    if (!scene.open(sceneId)) {
        return stopAfterRenderer();
    }

    {
        // How much of the shaders' packing the scene's loading didn't cover.
        auto waitStart = std::chrono::steady_clock::now();
        bool created = renderer.createStartupPipelines();
        auto waitedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
        if (!created) {
            TRACE_ERROR("Render", "StartupPipelinesFailed", "startup").text("a start-up pipeline can't be created; the reason is above");
            return stopAfterRenderer();
        }
        TRACE_EVENT("Render", "ShadersReady", "startup")
            .text(std::format("start-up pipelines created after waiting {:.0f} ms for their shaders", waitedMs))
            .field("waited_ms", waitedMs);
    }

    renderer.uploadRenderWorld(scene.renderWorld(), *scene.meshes(), *scene.materials());
    uint64_t uploadedRevision = scene.revision();
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
            if (inst.worldBounds.valid()) {
                sceneBounds.min = glm::min(sceneBounds.min, inst.worldBounds.min);
                sceneBounds.max = glm::max(sceneBounds.max, inst.worldBounds.max);
            }
        }
        if (sceneBounds.valid()) {
            cam.frame(sceneBounds, glm::radians(45.0f));
        }
    };
    frameSceneView();

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
        session.takeDue(frameCounter, dueCommands);
        for (const auto& c : dueCommands) {
            runViewScriptCommand(registry, c);
        }
        endpoint.drain();
        if (maxFrames > 0 && frameCounter > maxFrames) {
            quit = true;
        }
        auto nowTicks = SDL_GetTicksNS();
        auto dt = (float) (nowTicks - lastTicks) / 1.0e9f;
        lastTicks = nowTicks;

        int winW = 0;
        int winH = 0;
        SDL_GetWindowSizeInPixels(window, &winW, &winH);
        RhiExtent2D winExtent = {.width = (uint32_t) winW, .height = (uint32_t) winH};
        auto proj = glm::perspective(glm::radians(45.0f), (float) winW / (float) winH, 0.1f, 3000.0f);
        proj[1][1] *= -1.0f;

        // The purposes the View menu or a script turned on or off: the scene loads and draws them, and moves its revision on.
        if (scene.purposeShown(Purpose::Guide) != viewUI.showGuides()) {
            scene.setPurposeShown(Purpose::Guide, viewUI.showGuides());
        }
        if (scene.purposeShown(Purpose::Proxy) != viewUI.showProxies()) {
            scene.setPurposeShown(Purpose::Proxy, viewUI.showProxies());
        }
        if (scene.revision() != uploadedRevision) {
            uploadedRevision = scene.revision();
            renderThread.submitRenderUpload(RenderUpload{
                .world = scene.renderWorld(),
                .meshLib = scene.meshes(),
                .matLib = scene.materials(),
            });
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
        cam.update(keys, dt);

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
            cascadeCount = fitShadowCascades(viewUI.getShadowSettings(), cam.viewMatrix(), proj, 0.1f, 3000.0f, lightDir, cam.worldUp, sceneBounds, cascades);
        }
        std::array<glm::vec3, 8> frozenCorners;
        if (cullState.frozenActive) {
            frozenCorners = cullState.frozenCorners();
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
            scene.repack(scene.assetIds());
        } else if (!repack.ids.empty()) {
            scene.repack(repack.ids);
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
            renderer.requestTextureDump(dump->material, dump->level, "texture_" + std::to_string(dump->material) + "_L" + std::to_string(dump->level) + ".png");
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
    renderer.destroy();
    rhiDevice.destroy();
    SDL_DestroyWindow(window);
    SDL_Quit();

    // The endpoint goes last, so subscribers get the trace up to here: the last events are sent before the
    // connections close, waiting up to a second for a subscriber to take them.
    bool failed = failOnValidation && validationErrors > 0;
    if (failed) {
        TRACE_ERROR("Engine", "ValidationFailed", "view").text(std::format("validation errors: {}", validationErrors)).field("count", (int64_t) validationErrors);
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
