#include "camera.h"
#include "debugdraw.h"
#include "editorui.h"
#include "jobsystem.h"
#include "mesh.h"
#include "renderer.h"
#include "rendersnapshot.h"
#include "renderthread.h"
#include "renderworld.h"
#include "rhidevicevulkan.h"
#include "rhieditorui.h"
#include "rotategizmo.h"
#include "scalegizmo.h"
#include "scenequery.h"
#include "sceneupdater.h"
#include "translategizmo.h"
#include "usdrenderextractor.h"
#include "usdscene.h"

#include <SDL3/SDL.h>

#include <glm/gtc/matrix_transform.hpp>

#include <print>

auto main(int argc, char* argv[]) -> int {
    USDScene usdScene;
    USDRenderExtractor usdExtractor;
    SceneQuerySystem sceneQuery;
    MeshLibrary meshLib;
    MaterialLibrary matLib;
    RenderWorld renderWorld;
    PrimHandle selectedPrim;
    SceneUpdater sceneUpdater;

    if (argc >= 2) {
        if (!usdScene.open(argv[1])) {
            std::println(stderr, "Failed to open USD scene: {}", argv[1]);
            return 1;
        }
        usdScene.updateAssetBindings(meshLib, matLib);
        usdExtractor.extract(usdScene, meshLib, renderWorld);
        sceneQuery.rebuild(usdScene, meshLib);
    }

    // SDL init and window
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::println(stderr, "SDL_Init failed: {}", SDL_GetError());
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
        std::println(stderr, "SDL_CreateWindowWithProperties failed: {}", SDL_GetError());
        return 1;
    }
    SDL_DestroyProperties(windowProps);

    // Job system
    JobSystem::init();

    // RHI device
    RhiDeviceVulkan rhiDevice;
    if (!rhiDevice.init(window)) {
        return 1;
    }

    // Renderer
    Renderer renderer;
    if (!renderer.init(&rhiDevice, window)) {
        return 1;
    }
    renderer.uploadRenderWorld(renderWorld, meshLib, matLib);
    EditorUI editorUI;

    // Render thread
    RenderThread renderThread;
    renderThread.start(&renderer);

    DebugDraw debugDraw;

    // Camera — worldUp tracks the stage's authored up-axis so the controls stay
    // consistent whether we're looking at a Y-up or Z-up asset.
    auto cam = Camera{
        .position = glm::vec3(2.0f, 1.5f, 2.0f),
        .worldUp = usdScene.worldUp(),
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

    // Cached shared_ptrs to the libraries — handed to the render thread via
    // RenderUpload. Rebuilt only when the libs actually change (initial load,
    // openScene, or async batch swap), NOT on the per-frame transform fast path.
    auto cachedMeshLib = std::make_shared<const MeshLibrary>(meshLib);
    auto cachedMatLib = std::make_shared<const MaterialLibrary>(matLib);
    auto refreshCachedLibs = [&] {
        cachedMeshLib = std::make_shared<const MeshLibrary>(meshLib);
        cachedMatLib = std::make_shared<const MaterialLibrary>(matLib);
    };

    // Union the world bounds of all mesh instances and frame the camera on them.
    // Used at startup and after openScene so the initial view works for any up-axis
    // and asset scale.
    auto frameSceneView = [&] {
        AABB sceneBounds = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
        for (const auto& inst : renderWorld.meshInstances) {
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
    auto lastTicks = SDL_GetTicksNS();
    auto quit = false;
    while (!quit && !editorUI.wantsQuit()) {
        auto nowTicks = SDL_GetTicksNS();
        auto dt = (float) (nowTicks - lastTicks) / 1.0e9f;
        lastTicks = nowTicks;

        int winW = 0, winH = 0;
        SDL_GetWindowSizeInPixels(window, &winW, &winH);
        RhiExtent2D winExtent = {(uint32_t) winW, (uint32_t) winH};
        auto proj = glm::perspective(glm::radians(45.0f), (float) winW / (float) winH, 0.1f, 3000.0f);
        proj[1][1] *= -1.0f;

        bool sceneChanged = false;

        if (editorUI.hasPendingOpen()) {
            auto path = editorUI.consumePendingOpenPath();
            if (editorUI.openScene(path.c_str(), usdScene, usdExtractor, meshLib, matLib, renderWorld, sceneQuery, sceneUpdater, selectedPrim)) {
                refreshCachedLibs();
                cam.worldUp = usdScene.worldUp();
                frameSceneView();
                sceneChanged = true;
            }
        }

        if (usdScene.isOpen()) {
            auto r = sceneUpdater.update(usdScene, usdExtractor, renderWorld, meshLib, matLib, sceneQuery);
            if (r == SceneUpdateResult::Full) {
                refreshCachedLibs();
            }
            if (r != SceneUpdateResult::None) {
                sceneChanged = true;
            }
        }

        if (sceneChanged) {
            renderThread.submitRenderUpload(RenderUpload{
                .world = renderWorld,
                .meshLib = cachedMeshLib,
                .matLib = cachedMatLib,
            });
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            auto uiCaptured = renderer.editorui()->processEvent(&ev);

            if (ev.type == SDL_EVENT_QUIT) {
                std::println("Quitting");
                quit = true;
            }

            if (uiCaptured) {
                continue;
            }

            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_E && (ev.key.mod & SDL_KMOD_CTRL) != 0) {
                editorUI.togglePanels();
                continue;
            }

            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_Z && (ev.key.mod & SDL_KMOD_CTRL) != 0) {
                bool isRedo = (ev.key.mod & SDL_KMOD_SHIFT) != 0;
                auto cmds = isRedo ? sceneUpdater.undoStack().redo() : sceneUpdater.undoStack().undo();
                for (auto& c : cmds) {
                    sceneUpdater.addEdit(std::move(c));
                }
                continue;
            }

            // R: select parent of currently selected prim. Filter key-repeat so
            // holding R doesn't walk straight up the hierarchy at 25Hz.
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_R && (ev.key.mod & SDL_KMOD_CTRL) == 0 && !ev.key.repeat) {
                if ((bool) selectedPrim) {
                    if (const auto* rec = usdScene.getPrimRecord(selectedPrim); rec && rec->parent) {
                        selectedPrim = rec->parent;
                    }
                }
                continue;
            }

            // F: frame the selected prim in the camera view.
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F && (ev.key.mod & SDL_KMOD_CTRL) == 0) {
                if ((bool) selectedPrim) {
                    auto bb = sceneQuery.anchorBounds(usdScene, selectedPrim);
                    if (bb.valid()) {
                        cam.frame(bb, glm::radians(45.0f));
                    }
                }
                continue;
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

            // Active gizmo drag (translate, rotate, or scale — whichever is in progress).
            bool anyGizmoDragging = translateGizmo.isDragging() || rotateGizmo.isDragging() || scaleGizmo.isDragging();
            if (ev.type == SDL_EVENT_MOUSE_MOTION && anyGizmoDragging) {
                std::optional<Transform> newLocal;
                if (translateGizmo.isDragging()) {
                    newLocal = translateGizmo.dragUpdate(ev.motion.x, ev.motion.y, winExtent, cam.viewMatrix(), proj);
                } else if (rotateGizmo.isDragging()) {
                    newLocal = rotateGizmo.dragUpdate(ev.motion.x, ev.motion.y, winExtent, cam.viewMatrix(), proj);
                } else {
                    newLocal = scaleGizmo.dragUpdate(ev.motion.x, ev.motion.y, winExtent, cam.viewMatrix(), proj);
                }
                if (newLocal) {
                    sceneUpdater.addEdit({.type = SceneEditCommand::Type::SetTransform,
                                          .prim = selectedPrim,
                                          .transform = *newLocal,
                                          .purpose = SceneEditRequestContext::Purpose::Preview});
                }
                continue;
            }

            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT && anyGizmoDragging) {
                std::optional<Transform> finalLocal;
                const Transform* inverseHint = nullptr;
                if (translateGizmo.isDragging()) {
                    finalLocal = translateGizmo.dragUpdate(ev.button.x, ev.button.y, winExtent, cam.viewMatrix(), proj);
                    inverseHint = &translateGizmo.dragStartLocalTransform();
                    translateGizmo.release();
                } else if (rotateGizmo.isDragging()) {
                    finalLocal = rotateGizmo.dragUpdate(ev.button.x, ev.button.y, winExtent, cam.viewMatrix(), proj);
                    inverseHint = &rotateGizmo.dragStartLocalTransform();
                    rotateGizmo.release();
                } else {
                    finalLocal = scaleGizmo.dragUpdate(ev.button.x, ev.button.y, winExtent, cam.viewMatrix(), proj);
                    inverseHint = &scaleGizmo.dragStartLocalTransform();
                    scaleGizmo.release();
                }
                if (finalLocal && inverseHint) {
                    sceneUpdater.addEdit({.type = SceneEditCommand::Type::SetTransform,
                                          .prim = selectedPrim,
                                          .transform = *finalLocal,
                                          .inverseTransform = *inverseHint});
                }
                continue;
            }

            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
                float mx = ev.button.x;
                float my = ev.button.y;

                if (renderer.gizmoHitTest(mx, my, winExtent)) {
                    continue;
                }

                if ((bool) selectedPrim) {
                    auto tool = editorUI.activeTool();
                    if (const auto* xf = usdScene.getTransform(selectedPrim)) {
                        auto pivot = sceneQuery.anchorPivot(usdScene, selectedPrim);
                        auto anchor = pivot;
                        auto bb = sceneQuery.anchorBounds(usdScene, selectedPrim);
                        if (bb.valid() && !bb.contains(pivot)) {
                            anchor = (bb.min + bb.max) * 0.5f;
                        }
                        bool grabbed = false;
                        if (tool == EditorTool::Translate) {
                            grabbed = translateGizmo.tryGrab(mx, my, winExtent, cam.viewMatrix(), proj, anchor, xf->local, xf->world);
                        } else if (tool == EditorTool::Rotate) {
                            grabbed = rotateGizmo.tryGrab(mx, my, winExtent, cam.viewMatrix(), proj, anchor, xf->local, xf->world);
                        } else if (tool == EditorTool::Scale) {
                            grabbed = scaleGizmo.tryGrab(mx, my, winExtent, cam.viewMatrix(), proj, anchor, xf->local, xf->world);
                        }
                        if (grabbed) {
                            continue;
                        }
                    }
                }

                if (!usdScene.isOpen()) {
                    continue;
                }

                float ndcX = (2.0f * mx / (float) winW) - 1.0f;
                float ndcY = (2.0f * my / (float) winH) - 1.0f;
                auto invVP = glm::inverse(proj * cam.viewMatrix());
                auto nearPt = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
                auto farPt = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
                nearPt /= nearPt.w;
                farPt /= farPt.w;

                Ray ray = {.origin = glm::vec3(nearPt), .direction = glm::normalize(glm::vec3(farPt - nearPt))};
                RaycastHit hit;
                selectedPrim = sceneQuery.raycast(ray, 3000.0f, hit) ? hit.prim : PrimHandle{};
            }
        }

        const auto* keys = SDL_GetKeyboardState(nullptr);
        cam.update(keys, dt);

        editorUI.drawDebug(debugDraw, renderWorld, selectedPrim, sceneQuery, sceneUpdater, usdScene, cam.position, cam.worldUp);

        renderThread.setFrameGraphDebugEnabled(editorUI.getShowFrameGraphWindow());
        auto fgDebugSnap = renderThread.latestFrameGraphDebug();

        renderer.editorui()->beginFrame();
        editorUI.draw(window, usdScene, sceneUpdater, renderWorld, selectedPrim, sceneQuery, matLib, cam, std::move(fgDebugSnap));
        auto imguiSnapshot = renderer.editorui()->endFrame();

        float mouseX = 0, mouseY = 0;
        SDL_GetMouseState(&mouseX, &mouseY);

        const auto* selXf = ((bool) selectedPrim && usdScene.isOpen()) ? usdScene.getTransform(selectedPrim) : nullptr;
        auto activeTool = editorUI.activeTool();
        // Anchor at the prim's pivot when it falls within the anchor bounds;
        // fall back to bounds center when the pivot is decoupled from the
        // visible mesh. anchorPivot walks up reset-stack ancestors.
        glm::vec3 gizmoAnchor(0.0f);
        if (selXf) {
            gizmoAnchor = sceneQuery.anchorPivot(usdScene, selectedPrim);
            auto bb = sceneQuery.anchorBounds(usdScene, selectedPrim);
            if (bb.valid() && !bb.contains(gizmoAnchor)) {
                gizmoAnchor = (bb.min + bb.max) * 0.5f;
            }
        }
        translateGizmo.update(winExtent, cam.viewMatrix(), proj, cam.position, mouseX, mouseY, selXf && activeTool == EditorTool::Translate, gizmoAnchor);
        rotateGizmo.update(winExtent, cam.viewMatrix(), proj, cam.position, mouseX, mouseY, selXf && activeTool == EditorTool::Rotate, gizmoAnchor);
        scaleGizmo.update(winExtent, cam.viewMatrix(), proj, cam.position, mouseX, mouseY, selXf && activeTool == EditorTool::Scale, gizmoAnchor);

        RenderSnapshot snapshot = {
            .viewMatrix = cam.viewMatrix(),
            .projMatrix = proj,
            .worldUp = cam.worldUp,
            .windowWidth = winW,
            .windowHeight = winH,
            .mouseX = mouseX,
            .mouseY = mouseY,
            .showGizmo = editorUI.getShowGizmo(),
            .gbufferViewMode = static_cast<GBufferView>(editorUI.getGBufferViewMode()),
            .showBufferOverlay = editorUI.getShowBufferOverlay(),
            .showShadowOverlay = editorUI.getShowShadowOverlay(),
            .translateGizmoVerts = {translateGizmo.vertices().begin(), translateGizmo.vertices().end()},
            .rotateGizmoVerts = {rotateGizmo.vertices().begin(), rotateGizmo.vertices().end()},
            .scaleGizmoVerts = {scaleGizmo.vertices().begin(), scaleGizmo.vertices().end()},
            .debugData = debugDraw.data(),
            .imguiSnapshot = std::move(imguiSnapshot),
        };

        renderThread.submitSnapshot(std::move(snapshot));
    }

    renderThread.stop();
    JobSystem::shutdown();
    renderer.destroy();
    rhiDevice.destroy();
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
