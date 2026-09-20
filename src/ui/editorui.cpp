#include "editorui.h"
#include "profile.h"

#include "assetbrowserwindow.h"
#include "debugdraw.h"
#include "framegraphwindow.h"
#include "layerswindow.h"
#include "mainmenubar.h"
#include "propertieswindow.h"
#include "renderworld.h"
#include "scenequery.h"
#include "sceneupdater.h"
#include "scenewindow.h"
#include "toolswindow.h"
#include "undowindow.h"
#include "usdrenderextractor.h"
#include "usdscene.h"

#include <functional>
#include <print>
#include <utility>

auto EditorUI::togglePanels() -> void {
    bool target = !(showSceneWindow || showPropertiesWindow || showLayersWindow || showToolsWindow);
    showSceneWindow = target;
    showPropertiesWindow = target;
    showLayersWindow = target;
    showToolsWindow = target;
}

auto EditorUI::draw(
    SDL_Window* window,
    USDScene& usdScene,
    SceneUpdater& sceneUpdater,
    RenderWorld& renderWorld,
    PrimHandle& selectedPrim,
    const SceneQuerySystem& sceneQuery,
    const MaterialLibrary& matLib,
    Camera& camera,
    std::optional<FrameGraphDebugSnapshot> freshFrameGraphSnap,
    std::optional<RenderDebugSnapshot> freshRenderDebugSnap) -> void {
    if (freshFrameGraphSnap.has_value()) {
        fgLastSnapshot = std::move(freshFrameGraphSnap);
    }
    if (freshRenderDebugSnap.has_value()) {
        renderDebugLast = std::move(freshRenderDebugSnap);
    }
    MainMenuBarState menuState{
        .showSceneWindow = showSceneWindow,
        .showPropertiesWindow = showPropertiesWindow,
        .showLayersWindow = showLayersWindow,
        .showToolsWindow = showToolsWindow,
        .showUndoWindow = showUndoWindow,
        .showGrid = showGridFlag,
        .showOrigin = showOriginFlag,
        .showGizmo = showGizmoFlag,
        .showAABBs = showAABBsFlag,
        .showSelectedAABB = showSelectedAABBFlag,
        .showLightGizmos = showLightGizmosFlag,
        .depthPrepass = depthPrepassFlag,
        .gbufferView = gbufferViewMode,
        .showBufferOverlay = showBufferOverlayFlag,
        .showShadowOverlay = showShadowOverlayFlag,
        .antiAliasing = antiAliasingFlag,
        .showFrameGraph = showFrameGraphWindow,
        .showPerformance = showPerformanceWindow,
        .showRenderDebug = showRenderDebugWindow,
        .showCamera = showCameraWindow,
        .showCulling = showCullingWindow,
        .showAssetBrowser = showAssetBrowserWindow,
        .requestQuit = requestQuit,
        .pendingNewScene = pendingNewSceneFlag,
        .pendingOpenPath = pendingOpenPath,
        .pendingSavePath = pendingSavePath,
        .window = window,
        .sceneOpen = usdScene.isOpen(),
        .sceneUpdater = &sceneUpdater,
        .usdScene = &usdScene,
        .sceneQuery = &sceneQuery,
        .camera = &camera,
        .selectedPrim = &selectedPrim,
    };
    {
        PROFILE_ZONE("MenuBar");
        drawMainMenuBar(menuState);
    }
    {
        PROFILE_ZONE("LayersWindow");
        drawLayersWindow(showLayersWindow, sceneUpdater.isBlocked(), usdScene, sceneUpdater.edits());
    }
    {
        PROFILE_ZONE("SceneWindow");
        drawSceneWindow(showSceneWindow, sceneUpdater.isBlocked(), usdScene, renderWorld, selectedPrim, sceneState, sceneUpdater.edits());
    }
    {
        PROFILE_ZONE("PropertiesWindow");
        drawPropertiesWindow(showPropertiesWindow, sceneUpdater.isBlocked(), usdScene, selectedPrim, sceneQuery, matLib, sceneUpdater.edits(), propertiesState);
    }
    {
        PROFILE_ZONE("ToolsWindow");
        drawToolsWindow(showToolsWindow, activeToolValue);
    }
    {
        PROFILE_ZONE("UndoWindow");
        drawUndoWindow(showUndoWindow, sceneUpdater, usdScene);
    }
    {
        PROFILE_ZONE("FrameGraphWindow");
        drawFrameGraphWindow(showFrameGraphWindow, fgLastSnapshot, fgSelectedPass, fgSelectedResource);
    }
    {
        PROFILE_ZONE("PerformanceWindow");
        drawPerformanceWindow(showPerformanceWindow, performanceState);
    }
    {
        PROFILE_ZONE("RenderDebugWindow");
        RenderDebugViewFlags viewFlags = {
            .gbufferView = gbufferViewMode,
            .sampler = samplerSettings,
            .showBufferOverlay = showBufferOverlayFlag,
            .showShadowOverlay = showShadowOverlayFlag,
            .antiAliasing = antiAliasingFlag,
            .showGrid = showGridFlag,
            .showOrigin = showOriginFlag,
            .showGizmo = showGizmoFlag,
            .showAABBs = showAABBsFlag,
            .showLightGizmos = showLightGizmosFlag,
        };
        if (drawRenderDebugWindow(showRenderDebugWindow, renderDebugLast, viewFlags, usdScene, selectedPrim, renderDebugDraws)) {
            screenshotRequested = true;
        }
    }
    {
        PROFILE_ZONE("CameraWindow");
        drawCameraWindow(showCameraWindow, camera, cameraState);
    }
    {
        PROFILE_ZONE("CullingWindow");
        drawCullingWindow(showCullingWindow, {.enabled = cullEnabledFlag, .frozen = cullFrozenFlag, .showCulled = showCulledFlag, .instances = cullStatInstances, .culled = cullStatCulled});
    }
    {
        PROFILE_ZONE("AssetBrowserWindow");
        drawAssetBrowserWindow(showAssetBrowserWindow, usdScene, assetBrowser, sceneUpdater.edits());
    }
}

auto EditorUI::openScene(
    const char* path,
    USDScene& usdScene,
    USDRenderExtractor& usdExtractor,
    MeshLibrary& meshLib,
    MaterialLibrary& matLib,
    RenderWorld& renderWorld,
    SceneQuerySystem& sceneQuery,
    SceneUpdater& sceneUpdater,
    PrimHandle& selectedPrim) -> bool {
    sceneUpdater.waitIfBlocked();
    sceneUpdater.edits().clear();
    sceneUpdater.undoStack().clear();
    if (usdScene.isOpen()) {
        usdScene.close();
    }
    meshLib = {};
    matLib = {};
    renderWorld.clear();
    selectedPrim = {};

    if (!usdScene.open(path)) {
        std::println(stderr, "Failed to open: {}", path);
        return false;
    }
    usdScene.updateAssetBindings(meshLib, matLib);
    usdExtractor.extract(usdScene, meshLib, renderWorld);
    sceneQuery.rebuild(usdScene, meshLib);
    // Browser root is the CWD, set once at startup — don't touch it on scene change.
    // Just clear any stale selection from the previous scene.
    assetBrowser.selected.clear();
    showSceneWindow = true;
    showPropertiesWindow = true;
    showLayersWindow = true;
    showToolsWindow = true;
    showUndoWindow = true;
    return true;
}

auto EditorUI::newScene(
    USDScene& usdScene,
    USDRenderExtractor& usdExtractor,
    MeshLibrary& meshLib,
    MaterialLibrary& matLib,
    RenderWorld& renderWorld,
    SceneQuerySystem& sceneQuery,
    SceneUpdater& sceneUpdater,
    PrimHandle& selectedPrim) -> bool {
    sceneUpdater.waitIfBlocked();
    sceneUpdater.edits().clear();
    sceneUpdater.undoStack().clear();
    if (usdScene.isOpen()) {
        usdScene.close();
    }
    meshLib = {};
    matLib = {};
    renderWorld.clear();
    selectedPrim = {};

    if (!usdScene.newScene()) {
        std::println(stderr, "Failed to create new scene");
        return false;
    }
    usdScene.updateAssetBindings(meshLib, matLib);
    usdExtractor.extract(usdScene, meshLib, renderWorld);
    sceneQuery.rebuild(usdScene, meshLib);
    assetBrowser.selected.clear();
    showSceneWindow = true;
    showPropertiesWindow = true;
    showLayersWindow = true;
    showToolsWindow = true;
    showUndoWindow = true;
    return true;
}

auto EditorUI::drawDebug(
    DebugDraw& debugDraw,
    const RenderWorld& renderWorld,
    PrimHandle selectedPrim,
    const SceneQuerySystem& sceneQuery,
    const SceneUpdater& sceneUpdater,
    USDScene& usdScene,
    glm::vec3 cameraPos,
    glm::vec3 worldUp,
    std::span<const uint8_t> visible,
    const std::array<glm::vec3, 8>* frozenFrustum) -> void {
    debugDraw.newFrame();
    if (showGridFlag) {
        debugDraw.grid(cameraPos, worldUp, 1.0f, 50, {0.25f, 0.25f, 0.25f, 1.0f});
    }
    if (showOriginFlag) {
        debugDraw.sphere({0, 0, 0}, 0.1f, {1.0f, 0.9f, 0.2f, 1.0f}, 16);
    }
    if (showAABBsFlag) {
        for (const auto& inst : renderWorld.meshInstances) {
            if (inst.worldBounds.valid()) {
                debugDraw.box(inst.worldBounds, {0.0f, 1.0f, 0.0f, 1.0f});
            }
        }
    }
    // Culling debug: every instance AABB by its cull result, red for culled, green for
    // drawn, and the frozen frustum's twelve edges in yellow when the frustum is frozen.
    if (showCulledFlag && visible.size() == renderWorld.meshInstances.size()) {
        for (size_t i = 0; i < visible.size(); i++) {
            const auto& inst = renderWorld.meshInstances[i];
            if (!inst.worldBounds.valid()) {
                continue;
            }
            glm::vec4 color = visible[i] != 0 ? glm::vec4{0.2f, 1.0f, 0.2f, 1.0f} : glm::vec4{1.0f, 0.2f, 0.2f, 1.0f};
            debugDraw.box(inst.worldBounds, color);
        }
    }
    if (frozenFrustum != nullptr) {
        const auto& c = *frozenFrustum;
        glm::vec4 yellow = {1.0f, 0.9f, 0.2f, 1.0f};
        for (int i = 0; i < 4; i++) {
            int next = (i + 1) % 4;
            debugDraw.line(c[i], c[next], yellow);         // near ring
            debugDraw.line(c[4 + i], c[4 + next], yellow); // far ring
            debugDraw.line(c[i], c[4 + i], yellow);        // near to far
        }
    }
    if (showLightGizmosFlag && !renderWorld.lights.empty()) {
        // Camera-relative anchor: the sun floats a fixed distance in front of the camera
        // along the toward-light direction. Constant world size, so it reads the same at
        // any scene scale and never interacts with scene geometry. The gizmo visibly
        // follows the camera, which is the point — it's a directional compass, not an
        // object in the scene.
        constexpr float kSunDistance = 3.0f;
        constexpr float kDiscRadius = 0.25f;
        constexpr float kShaftLength = 0.75f;

        for (const auto& l : renderWorld.lights) {
            if (l.type != LightType::Directional) {
                continue;
            }
            // worldTransform[2] is the light's +Z axis — "toward the light" by USD convention.
            auto toward = glm::vec3(l.worldTransform[2]);
            if (glm::dot(toward, toward) < 1e-6f) {
                continue;
            }
            auto towardUnit = glm::normalize(toward);
            auto sunPos = cameraPos + towardUnit * kSunDistance;
            auto outgoing = -towardUnit;
            debugDraw.sunLight(sunPos, outgoing, kDiscRadius, kShaftLength, {1.0f, 0.85f, 0.2f, 1.0f});
        }
    }
    if (selectedPrim && showSelectedAABBFlag && !sceneUpdater.isBlocked()) {
        std::function<void(PrimHandle)> highlightSubtree = [&](PrimHandle h) {
            const auto* bc = sceneQuery.bounds().get(h);
            if (bc && bc->worldBounds.valid()) {
                debugDraw.box(bc->worldBounds, {1.0f, 0.0f, 0.6f, 1.0f});
            }
            auto child = usdScene.firstChild(h);
            while (child) {
                highlightSubtree(child);
                child = usdScene.nextSibling(child);
            }
        };
        highlightSubtree(selectedPrim);
    }
}

auto EditorUI::setOverlay(std::string_view name, bool on) -> bool {
    if (name == "grid") {
        showGridFlag = on;
    } else if (name == "origin") {
        showOriginFlag = on;
    } else if (name == "gizmo" || name == "gizmos") {
        showGizmoFlag = on;
    } else if (name == "aabbs") {
        showAABBsFlag = on;
    } else if (name == "lightgizmos") {
        showLightGizmosFlag = on;
    } else if (name == "buffer") {
        showBufferOverlayFlag = on;
    } else if (name == "shadow") {
        showShadowOverlayFlag = on;
    } else if (name == "aa") {
        antiAliasingFlag = on;
    } else {
        return false;
    }
    return true;
}
