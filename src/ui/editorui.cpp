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
        .debugView = debugViewMode,
        .showBufferOverlay = showBufferOverlayFlag,
        .showShadowOverlay = showShadowOverlayFlag,
        .antiAliasing = antiAliasingFlag,
        .showFrameGraph = showFrameGraphWindow,
        .showPerformance = showPerformanceWindow,
        .showRenderDebug = showRenderDebugWindow,
        .showCamera = showCameraWindow,
        .showCulling = showCullingWindow,
        .introspection = introspectionFlags,
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
        std::optional<FrameGraphCaptureRequest> captureRequest;
        drawFrameGraphWindow(showFrameGraphWindow, fgLastSnapshot, fgSelectedPass, fgSelectedResource, captureRequest);
        if (captureRequest.has_value()) {
            captureWindowState.pass = captureRequest->pass;
            captureWindowState.resource = captureRequest->resource;
            captureWindowState.trigger++;
            introspectionFlags.capture = true;
        }
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
            .shadow = shadowSettings,
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
    if (introspectionFlags.frameDebugger) {
        PROFILE_ZONE("FrameDebuggerWindow");
        auto primOfInstance = primOfEachInstance(renderWorld);
        auto primPath = [&](uint32_t instance) -> std::string {
            if (instance >= primOfInstance.size()) {
                return "(instance out of range)";
            }
            const auto* rec = usdScene.isOpen() ? usdScene.getPrimRecord(PrimHandle{primOfInstance[instance]}) : nullptr;
            return rec != nullptr ? rec->path : std::string("(unknown prim)");
        };
        auto openInCapture = [&](const std::string& pass, const std::string& resource) {
            captureWindowState.pass = pass;
            captureWindowState.resource = resource;
            captureWindowState.trigger++;
            introspectionFlags.capture = true;
        };
        drawFrameDebuggerWindow(introspectionFlags.frameDebugger, frameDebuggerState, primPath, openInCapture);
    }
    if (introspectionFlags.gpuScene) {
        PROFILE_ZONE("GpuSceneWindow");
        auto primOfInstance = primOfEachInstance(renderWorld);
        auto primPath = [&](uint32_t prim) -> std::string {
            const auto* rec = usdScene.isOpen() ? usdScene.getPrimRecord(PrimHandle{prim}) : nullptr;
            return rec != nullptr ? rec->path : std::string("(unknown prim)");
        };
        auto selectPrim = [&](uint32_t prim) {
            selectedPrim = PrimHandle{prim};
        };
        drawGpuSceneWindow(introspectionFlags.gpuScene, gpuSceneState, 1 + shadowCascadeCount, primOfInstance, primPath, selectPrim);
    }
    {
        PROFILE_ZONE("DebugViewWindow");
        updateDebugViewCursor(debugViewState);
        auto primOfInstance = primOfEachInstance(renderWorld);
        auto instancePath = [&](uint32_t instance) -> std::string {
            if (instance >= primOfInstance.size()) {
                return "(instance out of range)";
            }
            const auto* rec = usdScene.isOpen() ? usdScene.getPrimRecord(PrimHandle{primOfInstance[instance]}) : nullptr;
            return rec != nullptr ? rec->path : std::string("(unknown prim)");
        };
        drawDebugViewWindow(static_cast<DebugView>(debugViewMode), debugViewState, instancePath);
    }
    if (introspectionFlags.counters) {
        PROFILE_ZONE("CountersWindow");
        drawCountersWindow(introspectionFlags.counters, countersState);
    }
    if (introspectionFlags.capture) {
        PROFILE_ZONE("CaptureWindow");
        drawCaptureWindow(introspectionFlags.capture, fgLastSnapshot, captureWindowState);
    }
    if (introspectionFlags.memory) {
        PROFILE_ZONE("MemoryWindow");
        drawMemoryWindow(introspectionFlags.memory, renderDebugLast, memoryWindowState);
    }
    {
        PROFILE_ZONE("CameraWindow");
        drawCameraWindow(showCameraWindow, camera, cameraState);
    }
    {
        PROFILE_ZONE("CullingWindow");
        drawCullingWindow(showCullingWindow, {.enabled = cullEnabledFlag, .frozen = cullFrozenFlag, .showCulled = showCulledFlag, .overlayView = cullOverlayViewIndex, .showCascadeFrusta = showCascadeFrustaFlag, .instances = cullStatInstances, .culled = cullStatCulled, .cascades = shadowCascadeCount, .shadowCulled = shadowCulledStats, .shadowDrawn = shadowDrawnStats});
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
    const std::array<glm::vec3, 8>* frozenFrustum,
    std::span<const ShadowCascade> cascades) -> void {
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
    // Each shadow cascade's light frustum (an ortho box) in its own colour: red, green, blue,
    // yellow from near to far, the same colours as the cascades view.
    if (showCascadeFrustaFlag) {
        const std::array<glm::vec4, 4> colors = {{{1.0f, 0.3f, 0.3f, 1.0f}, {0.3f, 1.0f, 0.3f, 1.0f}, {0.3f, 0.5f, 1.0f, 1.0f}, {1.0f, 0.9f, 0.2f, 1.0f}}};
        for (size_t i = 0; i < cascades.size() && i < colors.size(); i++) {
            auto c = Frustum::corners(cascades[i].viewProj);
            for (int e = 0; e < 4; e++) {
                int next = (e + 1) % 4;
                debugDraw.line(c[e], c[next], colors[i]);
                debugDraw.line(c[4 + e], c[4 + next], colors[i]);
                debugDraw.line(c[e], c[4 + e], colors[i]);
            }
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
    } else if (name == "cascadefrusta") {
        showCascadeFrustaFlag = on;
    } else {
        return false;
    }
    return true;
}

auto EditorUI::setIntrospectionWindow(std::string_view name, bool on) -> bool {
    if (name == "memory") {
        introspectionFlags.memory = on;
    } else if (name == "capture") {
        introspectionFlags.capture = on;
    } else if (name == "framedebugger") {
        introspectionFlags.frameDebugger = on;
    } else if (name == "gpuscene") {
        introspectionFlags.gpuScene = on;
    } else if (name == "counters") {
        introspectionFlags.counters = on;
    } else if (name == "shaders") {
        introspectionFlags.shaders = on;
    } else {
        return false;
    }
    return true;
}

auto EditorUI::captureWatches() const -> std::vector<CaptureWatch> {
    std::vector<CaptureWatch> watches;
    if (introspectionFlags.capture) {
        if (auto watch = captureWindowWatch(captureWindowState); watch.has_value()) {
            watches.push_back(*watch);
        }
    }
    if (introspectionFlags.frameDebugger) {
        for (auto& watch : frameDebuggerWatches(frameDebuggerState)) {
            watches.push_back(std::move(watch));
        }
    }
    if (introspectionFlags.gpuScene) {
        for (auto& watch : gpuSceneWatches(gpuSceneState)) {
            watches.push_back(std::move(watch));
        }
    }
    if (auto watch = debugViewReadoutWatchFor(debugViewState, static_cast<DebugView>(debugViewMode)); watch.has_value()) {
        watches.push_back(*watch);
    }
    return watches;
}

auto EditorUI::onCaptureResult(CaptureResult result) -> void {
    if (result.id == captureWindowWatchId) {
        captureWindowState.result = std::move(result);
    } else if (result.id >= frameDebuggerCommandsWatch && result.id < frameDebuggerWatchBase + 1000) {
        frameDebuggerState.results[result.id] = std::move(result);
    } else if (result.id == debugViewReadoutWatch) {
        debugViewState.readout = std::move(result);
    } else if (const auto* resource = gpuSceneResourceOfWatch(result.id); resource != nullptr) {
        gpuSceneState.results[*resource] = std::move(result);
    }
}

auto EditorUI::onFrameDebugCapture(FrameDebugCapture capture) -> void {
    frameDebuggerState.capture = std::move(capture);
    frameDebuggerState.results.clear();
    frameDebuggerState.trigger++;
    if (frameDebuggerState.selectedPass >= (int) frameDebuggerState.capture->passes.size()) {
        frameDebuggerState.selectedPass = -1;
    }
}
