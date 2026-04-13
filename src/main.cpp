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

    // Camera
    auto cam = Camera{
        .position = glm::vec3(2.0f, 1.5f, 2.0f),
        .yaw = -135.0f,
        .pitch = -20.0f,
        .speed = 5.0f,
        .mouseSensitivity = 0.15f,
    };
    auto mouseCapture = false;

    renderer.initGizmos(&cam);
    TranslateGizmo translateGizmo;

    // Cached shared_ptrs to the libraries — handed to the render thread via
    // RenderUpload. Rebuilt only when the libs actually change (initial load,
    // openScene, or async batch swap), NOT on the per-frame transform fast path.
    auto cachedMeshLib = std::make_shared<const MeshLibrary>(meshLib);
    auto cachedMatLib = std::make_shared<const MaterialLibrary>(matLib);
    auto refreshCachedLibs = [&] {
        cachedMeshLib = std::make_shared<const MeshLibrary>(meshLib);
        cachedMatLib = std::make_shared<const MaterialLibrary>(matLib);
    };

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

            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_RIGHT) {
                mouseCapture = true;
            }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_RIGHT) {
                mouseCapture = false;
            }
            if (ev.type == SDL_EVENT_MOUSE_MOTION && mouseCapture) {
                cam.handleMouseMotion(ev.motion.xrel, ev.motion.yrel);
            }

            if (ev.type == SDL_EVENT_MOUSE_MOTION && translateGizmo.isDragging()) {
                if (auto newLocal = translateGizmo.dragUpdate(ev.motion.x, ev.motion.y, winExtent, cam.viewMatrix(), proj)) {
                    // Preview edit: cache-only, no USD write. The final position
                    // is committed below on mouse-up.
                    sceneUpdater.addEdit({.type = SceneEditCommand::Type::SetTransform,
                                          .prim = selectedPrim,
                                          .transform = *newLocal,
                                          .purpose = SceneEditRequestContext::Purpose::Preview});
                }
                continue;
            }

            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT && translateGizmo.isDragging()) {
                // Commit: one Authoring edit with the final position writes to the USD layer.
                // Carry the drag-start local as the inverse hint so the undo stack
                // records the pre-drag state, not the post-Preview cache value.
                if (auto finalLocal = translateGizmo.dragUpdate(ev.button.x, ev.button.y, winExtent, cam.viewMatrix(), proj)) {
                    sceneUpdater.addEdit({.type = SceneEditCommand::Type::SetTransform,
                                          .prim = selectedPrim,
                                          .transform = *finalLocal,
                                          .inverseTransform = translateGizmo.dragStartLocalTransform()});
                }
                translateGizmo.release();
                continue;
            }

            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
                float mx = ev.button.x;
                float my = ev.button.y;

                if (renderer.gizmoHitTest(mx, my, winExtent)) {
                    continue;
                }

                if ((bool) selectedPrim && editorUI.activeTool() == EditorTool::Translate) {
                    if (const auto* xf = usdScene.getTransform(selectedPrim)) {
                        auto anchor = glm::vec3(xf->world[3]);
                        if (auto* bc = sceneQuery.bounds().get(selectedPrim); bc && bc->worldBounds.valid()) {
                            anchor = (bc->worldBounds.min + bc->worldBounds.max) * 0.5f;
                        }
                        if (translateGizmo.tryGrab(mx, my, winExtent, cam.viewMatrix(), proj, anchor, xf->local, xf->world)) {
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

        editorUI.drawDebug(debugDraw, renderWorld, selectedPrim, sceneQuery, sceneUpdater, usdScene, cam.position);

        renderer.editorui()->beginFrame();
        editorUI.draw(window, usdScene, sceneUpdater, renderWorld, selectedPrim, sceneQuery, matLib);
        auto imguiSnapshot = renderer.editorui()->endFrame();

        float mouseX = 0, mouseY = 0;
        SDL_GetMouseState(&mouseX, &mouseY);

        const auto* selXf = ((bool) selectedPrim && usdScene.isOpen()) ? usdScene.getTransform(selectedPrim) : nullptr;
        bool translateActive = selXf != nullptr && editorUI.activeTool() == EditorTool::Translate;
        // Anchor at the selected prim's world bounds center when available, so
        // the gizmo lands on the visible mesh regardless of how the prim's
        // transform vs vertex coordinates split up its world position.
        glm::vec3 gizmoAnchor(0.0f);
        if (selXf) {
            gizmoAnchor = glm::vec3(selXf->world[3]);
            if (auto* bc = sceneQuery.bounds().get(selectedPrim); bc && bc->worldBounds.valid()) {
                gizmoAnchor = (bc->worldBounds.min + bc->worldBounds.max) * 0.5f;
            }
        }
        translateGizmo.update(winExtent, cam.viewMatrix(), proj, cam.position, mouseX, mouseY, translateActive, gizmoAnchor);

        RenderSnapshot snapshot = {
            .viewMatrix = cam.viewMatrix(),
            .projMatrix = proj,
            .windowWidth = winW,
            .windowHeight = winH,
            .mouseX = mouseX,
            .mouseY = mouseY,
            .showGizmo = editorUI.getShowGizmo(),
            .gbufferViewMode = static_cast<GBufferView>(editorUI.getGBufferViewMode()),
            .showBufferOverlay = editorUI.getShowBufferOverlay(),
            .translateGizmoVerts = {translateGizmo.vertices().begin(), translateGizmo.vertices().end()},
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
