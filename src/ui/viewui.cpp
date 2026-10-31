#include "viewui.h"

#include "debugdraw.h"
#include "framegraphwindow.h"
#include "profile.h"
#include "renderworld.h"
#include "viewmenubar.h"

#include "renderconfig.h"

#include <imgui.h>

#include <array>
#include <utility>

auto ViewUI::toggleShortcuts() -> void {
    auto pressed = [](ImGuiKeyChord chord) {
        return ImGui::Shortcut(chord, ImGuiInputFlags_RouteGlobal);
    };
    std::array<std::pair<ImGuiKeyChord, bool*>, 6> windows = {{
        {ImGuiMod_Ctrl | ImGuiKey_D, &cameraState.debug.on},
        {ImGuiMod_Ctrl | ImGuiKey_P, &showPerformanceWindow},
        {ImGuiMod_Ctrl | ImGuiKey_C, &showCameraWindow},
        {ImGuiMod_Ctrl | ImGuiKey_S, &showStreamingWindow},
        {ImGuiMod_Ctrl | ImGuiKey_L, &showLodWindow},
        {ImGuiMod_Ctrl | ImGuiKey_A, &showAssetsWindow},
    }};
    for (auto [chord, shown] : windows) {
        if (pressed(chord)) {
            *shown = !*shown;
        }
    }
    std::array<std::pair<ImGuiKey, DebugView>, 5> debugViews = {{
        {ImGuiKey_1, DebugView::Wireframe},
        {ImGuiKey_2, DebugView::TriangleSize},
        {ImGuiKey_3, DebugView::Overdraw},
        {ImGuiKey_4, DebugView::InstanceId},
        {ImGuiKey_5, DebugView::Streaming},
    }};
    for (auto [key, view] : debugViews) {
        if (pressed(key)) {
            (void) renderconfig::debugView.set(renderconfig::debugView.get() == (size_t) view ? (size_t) DebugView::None : (size_t) view);
        }
    }
    if (pressed(ImGuiMod_Ctrl | ImGuiKey_1)) {
        renderconfig::bufferOverlay.set(!renderconfig::bufferOverlay.get());
    }
    if (pressed(ImGuiMod_Ctrl | ImGuiKey_2)) {
        renderconfig::shadowOverlay.set(!renderconfig::shadowOverlay.get());
    }
    std::array<std::pair<ImGuiKeyChord, GBufferView>, 2> bufferViews = {{
        {ImGuiMod_Ctrl | ImGuiKey_3, GBufferView::MipLevel},
        {ImGuiMod_Ctrl | ImGuiKey_4, GBufferView::LodLevel},
    }};
    for (auto [chord, view] : bufferViews) {
        if (pressed(chord)) {
            (void) renderconfig::bufferView.set(renderconfig::bufferView.get() == (size_t) view ? (size_t) GBufferView::Lit : (size_t) view);
        }
    }
    if (pressed(ImGuiMod_Ctrl | ImGuiKey_5)) {
        PlainLit now = {
            .gbufferView = (int) renderconfig::bufferView.get(),
            .debugView = (int) renderconfig::debugView.get(),
            .bufferOverlay = renderconfig::bufferOverlay.get(),
            .shadowOverlay = renderconfig::shadowOverlay.get(),
        };
        bool plain = now.gbufferView == (int) GBufferView::Lit && now.debugView == (int) DebugView::None && !now.bufferOverlay && !now.shadowOverlay;
        auto apply = [](const PlainLit& views) {
            (void) renderconfig::bufferView.set((size_t) views.gbufferView);
            (void) renderconfig::debugView.set((size_t) views.debugView);
            renderconfig::bufferOverlay.set(views.bufferOverlay);
            renderconfig::shadowOverlay.set(views.shadowOverlay);
        };
        if (!plain) {
            beforePlainLit = now;
            apply(PlainLit{});
        } else if (beforePlainLit) {
            apply(*beforePlainLit);
            beforePlainLit.reset();
        }
    }
}

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
    toggleShortcuts();
    // The render settings the menu and the Render Debug window edit: their configuration values, copied here and written back after
    // both have drawn (a value written back unchanged doesn't count as a change).
    bool depthPrepass = renderconfig::depthPrepass.get();
    int gbufferView = (int) renderconfig::bufferView.get();
    int debugView = (int) renderconfig::debugView.get();
    bool bufferOverlay = renderconfig::bufferOverlay.get();
    bool shadowOverlay = renderconfig::shadowOverlay.get();
    bool antiAliasing = renderconfig::antiAliasing.get();
    auto sampler = renderconfig::samplerSettings();
    auto shadow = renderconfig::shadowSettings();
    auto ambient = renderconfig::ambient();
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
        .depthPrepass = depthPrepass,
        .gbufferView = gbufferView,
        .debugView = debugView,
        .showBufferOverlay = bufferOverlay,
        .showShadowOverlay = shadowOverlay,
        .antiAliasing = antiAliasing,
        .followGround = cameraState.modes.follow,
        .debugCamera = cameraState.debug.on,
        .debugCameraControlMain = cameraState.debug.controlMain,
        .showFrameGraph = showFrameGraphWindow,
        .showPerformance = showPerformanceWindow,
        .showConfig = showConfigWindow,
        .showStatusBar = showStatusBar,
        .showRenderDebug = showRenderDebugWindow,
        .showCamera = showCameraWindow,
        .showCulling = showCullingWindow,
        .showAssets = showAssetsWindow,
        .showLod = showLodWindow,
        .showStreaming = showStreamingWindow,
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
        PROFILE_ZONE("ConfigWindow");
        drawConfigWindow(showConfigWindow, configState);
    }
    {
        PROFILE_ZONE("RenderDebugWindow");
        RenderDebugViewFlags viewFlags = {
            .gbufferView = gbufferView,
            .sampler = sampler,
            .shadow = shadow,
            .ambient = ambient,
            .showBufferOverlay = bufferOverlay,
            .showShadowOverlay = shadowOverlay,
            .antiAliasing = antiAliasing,
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
    renderconfig::depthPrepass.set(depthPrepass);
    (void) renderconfig::bufferView.set((size_t) gbufferView);
    (void) renderconfig::debugView.set((size_t) debugView);
    renderconfig::bufferOverlay.set(bufferOverlay);
    renderconfig::shadowOverlay.set(shadowOverlay);
    renderconfig::antiAliasing.set(antiAliasing);
    renderconfig::setSamplerSettings(sampler);
    renderconfig::setShadowSettings(shadow);
    renderconfig::setAmbient(ambient);
    // An instance's prim path, looked up when a window names one: a table of every instance's prim costs milliseconds a frame in a large
    // scene.
    auto instancePath = [&](uint32_t instance) -> std::string {
        if (instance >= renderWorld.meshInstances.size()) {
            return "(instance out of range)";
        }
        const auto& meshInstance = renderWorld.meshInstances[instance];
        auto path = meshInstance.alive ? primPath(meshInstance.prim) : std::string();
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
        // Every instance's prim, built only while the window is open.
        auto primOfInstance = primOfEachInstance(renderWorld);
        drawGpuSceneWindow(introspectionFlags.gpuScene, gpuSceneState, 1 + shadowCascadeCount, primOfInstance, namePrim, selectPrim);
    }
    {
        PROFILE_ZONE("DebugViewWindow");
        updateDebugViewCursor(debugViewState);
        drawDebugViewWindow(static_cast<DebugView>(renderconfig::debugView.get()), debugViewState, instancePath);
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
        PROFILE_ZONE("StreamingWindow");
        drawStreamingWindow(showStreamingWindow, streamingState);
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
                           .lodLevelNotDrawn = cullStatLodNotDrawn,
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
        renderconfig::bufferOverlay.set(on);
    } else if (name == "shadow") {
        renderconfig::shadowOverlay.set(on);
    } else if (name == "aa") {
        renderconfig::antiAliasing.set(on);
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
    } else if (name == "streaming") {
        showStreamingWindow = on;
    } else if (name == "camera") {
        showCameraWindow = on;
    } else if (name == "renderdebug") {
        showRenderDebugWindow = on;
    } else if (name == "framegraph") {
        showFrameGraphWindow = on;
    } else if (name == "performance") {
        showPerformanceWindow = on;
    } else if (name == "config") {
        showConfigWindow = on;
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
    if (auto watch = debugViewReadoutWatchFor(debugViewState, static_cast<DebugView>(renderconfig::debugView.get())); watch.has_value()) {
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
