#include "viewui.h"

#include "debugdraw.h"
#include "framegraphwindow.h"
#include "profile.h"
#include "renderworld.h"
#include "viewmenubar.h"

auto ViewUI::draw(const PrimPathOf& primPath,
                  bool hasSelection,
                  const RenderWorld& renderWorld,
                  Camera& camera,
                  std::optional<FrameGraphDebugSnapshot> freshFrameGraphSnap,
                  std::optional<RenderDebugSnapshot> freshRenderDebugSnap) -> void {
    if (freshFrameGraphSnap.has_value()) {
        fgLastSnapshot = std::move(freshFrameGraphSnap);
    }
    if (freshRenderDebugSnap.has_value()) {
        renderDebugLast = std::move(freshRenderDebugSnap);
    }
    ViewMenuBarState menuState{
        .hasSelection = hasSelection,
        .tool = toolIndex,
        .requestDeselect = deselectRequested,
        .requestSelectParent = selectParentRequested,
        .requestFrameSelected = frameSelectedRequested,
        .showGrid = showGridFlag,
        .showOrigin = showOriginFlag,
        .showGizmo = showGizmoFlag,
        .showAABBs = showAABBsFlag,
        .showSelectedAABB = showSelectedAABBFlag,
        .showLightGizmos = showLightGizmosFlag,
        .showGuides = showGuidesFlag,
        .showProxies = showProxiesFlag,
        .depthPrepass = depthPrepassFlag,
        .gbufferView = gbufferViewMode,
        .debugView = debugViewMode,
        .showBufferOverlay = showBufferOverlayFlag,
        .showShadowOverlay = showShadowOverlayFlag,
        .antiAliasing = antiAliasingFlag,
        .showFrameGraph = showFrameGraphWindow,
        .showPerformance = showPerformanceWindow,
        .showStatusBar = showStatusBar,
        .showRenderDebug = showRenderDebugWindow,
        .showCamera = showCameraWindow,
        .showCulling = showCullingWindow,
        .showAssets = showAssetsWindow,
        .showLod = showLodWindow,
        .showSceneFlow = showSceneFlowWindow,
        .showEngineMap = showEngineMapWindow,
        .introspection = introspectionFlags,
        .requestQuit = requestQuit,
    };
    {
        PROFILE_ZONE("MenuBar");
        drawViewMenuBar(menuState);
    }
    if (showStatusBar) {
        PROFILE_ZONE("StatusBar");
        drawStatusBar(statusBarData);
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
            .ambient = ambient,
            .showBufferOverlay = showBufferOverlayFlag,
            .showShadowOverlay = showShadowOverlayFlag,
            .antiAliasing = antiAliasingFlag,
            .showGrid = showGridFlag,
            .showOrigin = showOriginFlag,
            .showGizmo = showGizmoFlag,
            .showAABBs = showAABBsFlag,
            .showLightGizmos = showLightGizmosFlag,
        };
        if (drawRenderDebugWindow(showRenderDebugWindow, renderDebugLast, viewFlags, primPath, renderDebugSelectedPrim, renderDebugDraws)) {
            screenshotRequested = true;
        }
    }
    auto primOfInstance = primOfEachInstance(renderWorld);
    auto instancePath = [&](uint32_t instance) -> std::string {
        if (instance >= primOfInstance.size()) {
            return "(instance out of range)";
        }
        auto path = primPath(primOfInstance[instance]);
        return !path.empty() ? path : std::string("(unknown prim)");
    };
    if (introspectionFlags.frameDebugger) {
        PROFILE_ZONE("FrameDebuggerWindow");
        auto openInCapture = [&](const std::string& pass, const std::string& resource) {
            captureWindowState.pass = pass;
            captureWindowState.resource = resource;
            captureWindowState.trigger++;
            introspectionFlags.capture = true;
        };
        drawFrameDebuggerWindow(introspectionFlags.frameDebugger, frameDebuggerState, instancePath, openInCapture);
    }
    if (introspectionFlags.gpuScene) {
        PROFILE_ZONE("GpuSceneWindow");
        auto namePrim = [&](uint32_t prim) -> std::string {
            auto path = primPath(prim);
            return !path.empty() ? path : std::string("(unknown prim)");
        };
        // Nothing is selected in the view yet: a click on a row selects nothing.
        auto selectPrim = [](uint32_t) {
        };
        drawGpuSceneWindow(introspectionFlags.gpuScene, gpuSceneState, 1 + shadowCascadeCount, primOfInstance, namePrim, selectPrim);
    }
    {
        PROFILE_ZONE("DebugViewWindow");
        updateDebugViewCursor(debugViewState);
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
        PROFILE_ZONE("AssetsWindow");
        drawAssetsWindow(showAssetsWindow, assetLog, assetsState);
    }
    {
        PROFILE_ZONE("LodWindow");
        drawLodWindow(showLodWindow, lodState);
    }
    {
        PROFILE_ZONE("SceneFlowWindow");
        drawSceneFlowWindow(showSceneFlowWindow, sceneFlowState);
    }
    {
        PROFILE_ZONE("EngineMapWindow");
        // A "Map" link in another window: open the map at its node.
        if (auto focus = takeEngineMapFocus(); !focus.empty()) {
            showEngineMapWindow = true;
            engineMapState.selected = focus;
            engineMapState.scrollToSelected = true;
            engineMapState.focusWindow = true;
        }
        drawEngineMapWindow(showEngineMapWindow, engineMapState, [this](const char* window) { setIntrospectionWindow(window, true); });
    }
    {
        PROFILE_ZONE("CullingWindow");
        drawCullingWindow(showCullingWindow,
                          {.enabled = cullEnabledFlag,
                           .frozen = cullFrozenFlag,
                           .showCulled = showCulledFlag,
                           .overlayView = cullOverlayViewIndex,
                           .showCascadeFrusta = showCascadeFrustaFlag,
                           .instances = cullStatInstances,
                           .culled = cullStatCulled,
                           .cascades = shadowCascadeCount,
                           .shadowCulled = shadowCulledStats,
                           .shadowDrawn = shadowDrawnStats,
                           .trianglesDrawn = cullTrianglesDrawn,
                           .trianglesScene = cullTrianglesScene,
                           .shadowTriangles = cullTrianglesCascades});
    }
}

auto ViewUI::drawDebug(DebugDraw& debugDraw,
                       const RenderWorld& renderWorld,
                       glm::vec3 cameraPos,
                       glm::vec3 worldUp,
                       std::span<const uint8_t> visible,
                       const std::array<glm::vec3, 8>* frozenFrustum,
                       std::span<const ShadowCascade> cascades,
                       const AABB* selectedBounds) -> void {
    debugDraw.newFrame();
    if (showGridFlag) {
        debugDraw.grid(cameraPos, worldUp, 1.0f, 50, {0.25f, 0.25f, 0.25f, 1.0f});
    }
    if (showOriginFlag) {
        debugDraw.sphere({0, 0, 0}, 0.1f, {1.0f, 0.9f, 0.2f, 1.0f}, 16);
    }
    if (showAABBsFlag) {
        for (const auto& inst : renderWorld.meshInstances) {
            if (inst.alive && inst.worldBounds.valid()) {
                debugDraw.box(inst.worldBounds, {0.0f, 1.0f, 0.0f, 1.0f});
            }
        }
    }
    // Culling debug: every instance AABB by its cull result, red for culled, green for drawn, and the frozen frustum's twelve edges in
    // yellow when the frustum is frozen.
    if (showCulledFlag && visible.size() == renderWorld.meshInstances.size()) {
        for (size_t i = 0; i < visible.size(); i++) {
            const auto& inst = renderWorld.meshInstances[i];
            if (!inst.alive || !inst.worldBounds.valid()) {
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
            debugDraw.line(c[i], c[next], yellow);
            debugDraw.line(c[4 + i], c[4 + next], yellow);
            debugDraw.line(c[i], c[4 + i], yellow);
        }
    }
    // Each shadow cascade's light frustum (an ortho box) in its own colour: red, green, blue, yellow from near to far, the same colours
    // as the cascades view.
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
        // The sun floats a fixed distance in front of the camera along the toward-light direction: constant world size, so it reads the
        // same at any scene scale and never meets scene geometry. A directional compass, not an object in the scene.
        constexpr float sunDistance = 3.0f;
        constexpr float discRadius = 0.25f;
        constexpr float shaftLength = 0.75f;
        for (const auto& l : renderWorld.lights) {
            if (l.type != LightType::Directional) {
                continue;
            }
            // worldTransform[2] is the light's +Z axis: toward the light, by USD's convention.
            auto toward = glm::vec3(l.worldTransform[2]);
            if (glm::dot(toward, toward) < 1e-6f) {
                continue;
            }
            auto towardUnit = glm::normalize(toward);
            auto sunPos = cameraPos + towardUnit * sunDistance;
            debugDraw.sunLight(sunPos, -towardUnit, discRadius, shaftLength, {1.0f, 0.85f, 0.2f, 1.0f});
        }
    }
    if (selectedBounds != nullptr && showSelectedAABBFlag && selectedBounds->valid()) {
        debugDraw.box(*selectedBounds, {1.0f, 0.0f, 0.6f, 1.0f});
    }
}

auto ViewUI::takeEditRequest() -> ViewEditRequest {
    if (std::exchange(deselectRequested, false)) {
        return ViewEditRequest::Deselect;
    }
    if (std::exchange(selectParentRequested, false)) {
        return ViewEditRequest::SelectParent;
    }
    if (std::exchange(frameSelectedRequested, false)) {
        return ViewEditRequest::FrameSelected;
    }
    return ViewEditRequest::None;
}

auto ViewUI::setOverlay(std::string_view name, bool on) -> bool {
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

auto ViewUI::setIntrospectionWindow(std::string_view name, bool on) -> bool {
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
    } else if (name == "culling") {
        showCullingWindow = on;
    } else if (name == "assets") {
        showAssetsWindow = on;
    } else if (name == "lod") {
        showLodWindow = on;
    } else if (name == "sceneflow") {
        showSceneFlowWindow = on;
    } else if (name == "enginemap") {
        showEngineMapWindow = on;
    } else if (name == "camera") {
        showCameraWindow = on;
    } else if (name == "renderdebug") {
        showRenderDebugWindow = on;
    } else if (name == "framegraph") {
        showFrameGraphWindow = on;
    } else if (name == "performance") {
        showPerformanceWindow = on;
    } else {
        return false;
    }
    return true;
}

auto ViewUI::captureWatches() const -> std::vector<CaptureWatch> {
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

auto ViewUI::onCaptureResult(CaptureResult result) -> void {
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

auto ViewUI::onFrameDebugCapture(FrameDebugCapture capture) -> void {
    frameDebuggerState.capture = std::move(capture);
    frameDebuggerState.results.clear();
    frameDebuggerState.trigger++;
    if (frameDebuggerState.selectedPass >= (int) frameDebuggerState.capture->passes.size()) {
        frameDebuggerState.selectedPass = -1;
    }
}

auto ViewUI::takeTextureInspectRequest() -> std::optional<TextureInspectRequest> {
    if (!renderDebugDraws.inspectChanged) {
        return std::nullopt;
    }
    renderDebugDraws.inspectChanged = false;
    return renderDebugDraws.inspect;
}

auto ViewUI::takeTextureDumpRequest() -> std::optional<TextureInspectRequest> {
    if (!renderDebugDraws.dumpRequested) {
        return std::nullopt;
    }
    renderDebugDraws.dumpRequested = false;
    return renderDebugDraws.inspect;
}
