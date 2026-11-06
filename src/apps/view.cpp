#include "assetclient.h"
#include "assetid.h"
#include "gizmorender.h"
#include "ground.h"
#include "imguibackendvulkan.h"
#include "jobsystem.h"
#include "profile.h"
#include "renderconfig.h"
#include "renderdoccapture.h"
#include "renderer.h"
#include "renderthread.h"
#include "rendertools.h"
#include "rhidevicevulkan.h"
#include "rpcendpoint.h"
#include "rpcregistry.h"
#include "rpcverbs.h"
#include "sessionscript.h"
#include "toolcommands.h"
#include "toolshooks.h"
#include "trace.h"
#include "uicommands.h"
#include "uihooks.h"
#include "uirender.h"
#include "viewcommands.h"
#include "viewdumps.h"
#include "viewframe.h"
#include "viewhooks.h"
#include "viewscene.h"
#include "viewui.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <glm/glm.hpp>

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

// The window layer's input, as the view's controls take it: the right button held for mouse look, the keys as a camera move, Left Alt held
// for orbiting, and the window's size as the trace last reported it.
struct WindowInput {
    int tracedWidth = 0;
    int tracedHeight = 0;
    bool mouseLook = false;
    CameraMove move;
    bool orbitHeld = false;
};

// SDL's events this frame: quitting and resizing; then the hooks (the UI's ImGui and shortcuts, the tools' gizmos and keys), which may take
// an event; then the view's controls. Z deselects, R selects the parent, F frames the selection, G toggles following the ground, Left Alt
// starts orbiting, the right button looks, the wheel sets the fly speed, a left click picks. Then the keys held, as a camera move.
auto pollInput(SDL_Window* window, ViewFrame& frame, ViewHooks& hooks, WindowInput& input) -> void {
    static const uint32_t pollZoneId = profile::registerName("PollEvents");
    profile::beginZone(pollZoneId);
    uint64_t polledEvents = 0;
    // A framebuffer pixel from a point in the window (they differ on high-density displays).
    auto toPixel = [&](float pointX, float pointY) -> glm::uvec2 {
        int pointsW = 0;
        int pointsH = 0;
        SDL_GetWindowSize(window, &pointsW, &pointsH);
        return {(uint32_t) std::max(0.0f, pointX * (float) frame.width / (float) std::max(pointsW, 1)),
                (uint32_t) std::max(0.0f, pointY * (float) frame.height / (float) std::max(pointsH, 1))};
    };
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        polledEvents++;
        if (ev.type == SDL_EVENT_QUIT) {
            TRACE_EVENT("Engine", "QuitRequested", "view").text("quitting");
            frame.quit = true;
        }
        if (ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
            bool sizeChanged = ev.window.data1 != input.tracedWidth || ev.window.data2 != input.tracedHeight;
            if (sizeChanged) {
                input.tracedWidth = ev.window.data1;
                input.tracedHeight = ev.window.data2;
                TRACE_EVENT("Engine", "WindowResized", "window")
                    .text(std::format("window resized to {}x{} pixels", ev.window.data1, ev.window.data2))
                    .field("width", ev.window.data1)
                    .field("height", ev.window.data2);
            }
        }
        if (hooks.event(frame, ev)) {
            continue;
        }

        bool plainKey = ev.type == SDL_EVENT_KEY_DOWN && (ev.key.mod & SDL_KMOD_CTRL) == 0 && !ev.key.repeat;
        if (plainKey && ev.key.key == SDLK_Z) {
            frame.deselect();
            continue;
        }
        if (plainKey && ev.key.key == SDLK_R) {
            frame.selectParent();
            continue;
        }
        if (plainKey && ev.key.key == SDLK_F) {
            frame.frameSelected();
            continue;
        }
        if (plainKey && ev.key.key == SDLK_G) {
            viewconfig::follow.set(!viewconfig::follow.get());
            continue;
        }
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_LALT && !ev.key.repeat) {
            float pointX = 0.0f;
            float pointY = 0.0f;
            SDL_GetMouseState(&pointX, &pointY);
            frame.startOrbit(toPixel(pointX, pointY));
            continue;
        }

        if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_RIGHT) {
            input.mouseLook = true;
        }
        if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_RIGHT) {
            input.mouseLook = false;
        }
        if (ev.type == SDL_EVENT_MOUSE_MOTION && input.mouseLook) {
            frame.look(ev.motion.xrel, ev.motion.yrel);
        }
        if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
            frame.wheel(ev.wheel.y);
            continue;
        }
        if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
            frame.pick(toPixel(ev.button.x, ev.button.y));
        }
    }
    profile::zoneValue(polledEvents);
    profile::endZone();

    // The keys as a camera move: WASD across, E/Q up and down, Shift fast; none while Ctrl is held, which the shortcuts use.
    const auto* keys = SDL_GetKeyboardState(nullptr);
    input.move = {};
    if (!keys[SDL_SCANCODE_LCTRL] && !keys[SDL_SCANCODE_RCTRL]) {
        input.move = {
            .forward = (keys[SDL_SCANCODE_W] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_S] ? 1.0f : 0.0f),
            .right = (keys[SDL_SCANCODE_D] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_A] ? 1.0f : 0.0f),
            .up = (keys[SDL_SCANCODE_E] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_Q] ? 1.0f : 0.0f),
            .fast = keys[SDL_SCANCODE_LSHIFT],
        };
    }
    input.orbitHeld = keys[SDL_SCANCODE_LALT];
    SDL_GetMouseState(&frame.mouse.x, &frame.mouse.y);
}

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
    bool rpcEnabled = true; // --no-rpc: no endpoint, no discovery file
    // --no-wait-resident: script frames (and --frames, --screenshot) count from start-up instead of from the scene first being resident.
    bool waitForResident = true;
    SessionScript session;
    std::string sessionError;
    std::string startCamera;
    std::string startFrameTarget;
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
            (void) renderconfig::geometryLimitMiB.set((int64_t) std::stoull(flagValue(arg, "--geometry-limit=")));
        } else if (arg.starts_with("--frames=")) {
            maxFrames = std::stoull(flagValue(arg, "--frames="));
        } else if (arg.starts_with("--camera=")) {
            // A pose, or the path of a camera the scene authors; either before the scene loads, outside the script's clock.
            startCamera = flagValue(arg, "--camera=");
        } else if (arg.starts_with("--camera-frame=")) {
            // What the camera frames while the scene arrives: the scene, or what is drawn at and under a prim.
            startFrameTarget = flagValue(arg, "--camera-frame=");
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
    // What joins the renderer's frame from outside its core, in order: the instrumentation, the tools' gizmos, the UI (last on the
    // backbuffer). The UI shows the renderer's previews.
    RenderTools renderTools;
    GizmoRender gizmoRender;
    UiRender uiRender(imguiBackend);
    renderer.addExtension(renderTools);
    renderer.addExtension(gizmoRender);
    renderer.addExtension(uiRender);
    renderer.setPreviewTextures(&imguiBackend);
    renderTools.setValidationEnabled(enableValidation);
    renderTools.setScreenshotsShowUi(screenshotsShowUi);
    StatusBarGatherer statusBar;
    auto rendererInitStart = std::chrono::steady_clock::now();
    if (!renderer.init(&rhiDevice, &assetClient, initialExtent)) {
        TRACE_ERROR("Render", "RendererInitFailed", "renderer").text("renderer init failed");
        rhiDevice.waitIdle();
        return failAfterInit();
    }
    {
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rendererInitStart).count();
        TRACE_EVENT("Render", "RendererInitialized", "renderer").text(std::format("renderer initialized in {:.0f} ms", ms)).field("ms", ms);
    }
    renderer.setGeometryLimit((uint64_t) renderconfig::geometryLimitMiB.get() * 1024 * 1024);

    // The passes have registered their pipelines: their shaders go out in one request, and each pipeline is made when its shaders arrive.
    // Nothing waits for them: the frame is the clear colour and the UI until the lit path's pipelines are there.
    renderer.requestShaders();

    // The scene, composed from its packs as they arrive; no scene argument is an empty scene.
    ViewScene scene(assetClient);
    scene.open(sceneId);

    // Render thread
    RenderThread renderThread;
    renderThread.start(&renderer);

    // The engine's frame: the camera (its up axis the scene's), the selection, residency, culling, the snapshot.
    ViewFrame frame(scene, renderer, renderThread, assetClient);

    // The tools and the UI join the frame through their hooks; the UI's windows draw and edit the tools' state and the camera modes'.
    ToolsState tools;
    ViewUI viewUI;
    if (positional.size() > 1) {
        viewUI.setBookmarkPath(std::string(positional[1]) + ".cameras.txt");
    }
    viewUI.setAssetDrawCount([&scene](const std::string& assetId) { return scene.drawCount(assetId); });
    viewUI.bindTools(tools, frame.cameraModes);
    ToolsHooks toolsHooks(tools, gizmoRender);
    UiHooks uiHooks({
        .ui = viewUI,
        .tools = toolsHooks,
        .imgui = imguiBackend,
        .uiRender = uiRender,
        .renderTools = renderTools,
        .device = rhiDevice,
        .renderDoc = renderDoc,
    });
    ViewHooks& hooks = uiHooks;

    // Answers waiting for the scene to be resident (view.waitResident, view.lod.set, view.repack, view.purposes), and render-LOD records
    // waiting for the groups' states to be read back.
    std::vector<ResidentWaiter> residentWaiters;
    std::vector<RenderLodRequest> renderLodRequests;
    ScriptClock script = {.waitForResident = waitForResident, .lastFrame = maxFrames};
    // Dumps and captures that finish frames later, for script verbs and RPC calls alike.
    ViewDumps dumps(
        [&scene]() -> const RenderWorld& { return scene.renderWorld(); },
        [&scene](uint32_t prim) -> std::string { return scene.primPath(prim); },
        frame.latestCull,
        rhiDevice.limits().pipelineStatistics);

    // Every command is an RPC method; script lines and live calls run the same code.
    RpcRegistry registry;
    registerRpcBuiltins(registry, "view");
    ViewContext viewContext = {
        .cam = frame.cam,
        .scene = scene,
        .selectedPath = frame.selectedPath,
        .picking = frame.picking,
        .goTo = frame.goTo,
        .renderer = renderer,
        .renderThread = renderThread,
        .renderDoc = renderDoc,
        .renderTools = renderTools,
        .dumps = dumps,
        .latestCull = frame.latestCull,
        .assets = assetClient,
        .frameSceneView = [&frame] { frame.frameScene(); },
        .quit = frame.quit,
        .frameCounter = frame.frameCounter,
        .residentWaiters = residentWaiters,
        .residency = frame.residency,
        .script = script,
        .renderLodRequests = renderLodRequests,
        .viewAspect = frame.viewAspect,
        .lodPops = frame.lodPops,
        .flight = frame.flight,
        .cameraModes = frame.cameraModes,
        .startCamera = frame.startCamera,
        .autoFrame = frame.autoFrame,
        .sceneLabel = positional.size() >= 2 ? std::string(positional[1]) : std::string("(new scene)"),
    };
    // Each layer adds its methods, script verbs and records: the engine's, the tools', the UI's.
    RpcVerbs verbs;
    RpcRecords records;
    registerViewCommands(registry, verbs, records, viewContext);
    registerToolCommands(registry, verbs, records, viewContext, tools);
    registerUiCommands(registry, verbs, records, viewUI);
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

    // The start camera: a pose at once, a camera the scene authors when the scene names it (ViewFrame::startCamera). Either ends the
    // framing, so the scene loads what that camera sees and the script's clock starts there.
    if (startCamera.starts_with("/")) {
        frame.startCamera = startCamera;
    } else if (!startCamera.empty()) {
        verbs.run(registry, "camera", startCamera);
        frame.autoFrame = false;
    }
    if (startFrameTarget != "scene") {
        frame.frameTarget = startFrameTarget;
    }

    // Main loop: the engine's steps in order, the commands after the scene update, the hooks between the steps.
    profile::registerThread("Main");
    WindowInput input = {.tracedWidth = tracedWindowWidth, .tracedHeight = tracedWindowHeight};
    TRACE_EVENT("Engine", "MainLoopStarted", "view").text(std::format("main loop started, {:.0f} ms after start", trace::msSinceStart())).field("since_start_ms", trace::msSinceStart());
    while (!frame.quit) {
        PROFILE_FRAME_MARK();
        PROFILE_ZONE("MainFrame");
        frame.updateScene();
        hooks.afterScene(frame);
        runFrameCommands(viewContext, session, registry, verbs, endpoint);
        frame.advanceTime(SDL_GetTicksNS());
        frame.resetCameras();
        hooks.cameras(frame);

        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        frame.setViewport(width, height);
        frame.sendSceneInputs();
        frame.updateResidency();
        answerResidentWaiters(viewContext);

        pollInput(window, frame, hooks, input);
        frame.moveCamera(input.move, input.orbitHeld);
        frame.cull();
        // Render-LOD records answer once a readback carries every group's state; while one waits, the frames read them back.
        frame.readLodStates = serviceRenderLodRequests(viewContext);
        frame.countLodPops();
        frame.fitCascades();
        hooks.afterCamera(frame);

        frame.debugDraw.newFrame();
        hooks.debugDraw(frame);
        frame.drawCameraModes();

        frame.readbacks = {};
        renderTools.startFrame();
        renderTools.request().renderDebug = forceRenderDebug;
        hooks.wantReadbacks(frame);
        dumps.wantReadbacks(frame.readbacks, renderTools);
        frame.readBack();
        dumps.takeReadbacks(frame.readbacks, renderTools);
        hooks.takeReadbacks(frame);

        hooks.drawUi(frame);

        auto snapshot = frame.buildSnapshot();
        renderTools.fillSnapshot(snapshot);
        hooks.fillSnapshot(frame, snapshot);
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
        .text(std::format("exiting after {} frames", frame.frameCounter))
        .field("frames", (int64_t) frame.frameCounter)
        .field("validation_errors", (int64_t) validationErrors);
    endpoint.finishTrace(std::chrono::seconds(1));
    endpoint.stop();

    if (failed) {
        return 2;
    }
    return 0;
}
