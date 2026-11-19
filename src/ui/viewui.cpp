#include "viewui.h"

#include "debugdraw.h"
#include "framegraphwindow.h"
#include "profile.h"
#include "renderworld.h"
#include "viewmenubar.h"

#include "rendercvars.h"

#include <imgui.h>

#include <array>
#include <utility>

auto ViewUI::toggleShortcuts() -> void {
    auto pressed = [](ImGuiKeyChord chord) {
        return ImGui::Shortcut(chord, ImGuiInputFlags_RouteGlobal);
    };
    std::array<std::pair<ImGuiKeyChord, bool*>, 5> windows = {{
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
    if (pressed(ImGuiMod_Ctrl | ImGuiKey_D)) {
        if (auto* debugCamera = cvarBool("tools.debugcamera.on")) {
            debugCamera->set(!debugCamera->get());
        }
    }
    if (pressed(ImGuiMod_Ctrl | ImGuiKey_B)) {
        if (auto* budgetOverlay = cvarBool("ui.overlay.budget")) {
            budgetOverlay->set(!budgetOverlay->get());
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
            (void) rendercvars::debugView.set(rendercvars::debugView.get() == (size_t) view ? (size_t) DebugView::None : (size_t) view);
        }
    }
    if (pressed(ImGuiMod_Ctrl | ImGuiKey_1)) {
        rendercvars::bufferOverlay.set(!rendercvars::bufferOverlay.get());
    }
    if (pressed(ImGuiMod_Ctrl | ImGuiKey_2)) {
        rendercvars::shadowOverlay.set(!rendercvars::shadowOverlay.get());
    }
    std::array<std::pair<ImGuiKeyChord, ViewMode>, 2> viewModes = {{
        {ImGuiMod_Ctrl | ImGuiKey_3, ViewMode::MipLevel},
        {ImGuiMod_Ctrl | ImGuiKey_4, ViewMode::LodLevel},
    }};
    for (auto [chord, view] : viewModes) {
        if (pressed(chord)) {
            (void) rendercvars::viewMode.set(rendercvars::viewMode.get() == (size_t) view ? (size_t) ViewMode::Lit : (size_t) view);
        }
    }
    if (pressed(ImGuiMod_Ctrl | ImGuiKey_5)) {
        PlainLit now = {
            .viewMode = (int) rendercvars::viewMode.get(),
            .debugView = (int) rendercvars::debugView.get(),
            .bufferOverlay = rendercvars::bufferOverlay.get(),
            .shadowOverlay = rendercvars::shadowOverlay.get(),
        };
        bool plain = now.viewMode == (int) ViewMode::Lit && now.debugView == (int) DebugView::None && !now.bufferOverlay && !now.shadowOverlay;
        auto apply = [](const PlainLit& views) {
            (void) rendercvars::viewMode.set((size_t) views.viewMode);
            (void) rendercvars::debugView.set((size_t) views.debugView);
            rendercvars::bufferOverlay.set(views.bufferOverlay);
            rendercvars::shadowOverlay.set(views.shadowOverlay);
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
    // The render settings the menu and the Render Debug window edit: their cvars, copied here and written back after
    // both have drawn (a value written back unchanged doesn't count as a change).
    bool depthPrepass = rendercvars::depthPrepass.get();
    bool showGrid = toolscvars::overlayGrid.get();
    bool showOrigin = toolscvars::overlayOrigin.get();
    bool showGizmo = toolscvars::overlayGizmos.get();
    bool showAabbs = toolscvars::overlayAabbs.get();
    bool showSelectedAabb = toolscvars::overlaySelectedAabb.get();
    bool showLightGizmos = toolscvars::overlayLightGizmos.get();
    auto* guides = cvarBool("scene.purposes.guide");
    auto* proxies = cvarBool("scene.purposes.proxy");
    bool showGuides = guides != nullptr && guides->get();
    bool showProxies = proxies != nullptr && proxies->get();
    bool cullEnabled = rendercvars::cullingEnabled.get();
    bool cullFrozen = rendercvars::cullingFrozen.get();
    bool occlusion = rendercvars::cullingOcclusion.get();
    bool showCulled = toolscvars::overlayCulled.get();
    int culledView = (int) toolscvars::overlayCulledView.get();
    bool cascadeFrusta = toolscvars::overlayCascadeFrusta.get();
    int viewMode = (int) rendercvars::viewMode.get();
    int debugView = (int) rendercvars::debugView.get();
    bool bufferOverlay = rendercvars::bufferOverlay.get();
    bool shadowOverlay = rendercvars::shadowOverlay.get();
    bool antiAliasing = rendercvars::antiAliasing.get();
    auto sampler = rendercvars::samplerSettings();
    auto shadow = rendercvars::shadowSettings();
    auto ambient = rendercvars::ambient();
    ViewMenuBarState menuState{
        .hasSelection = hasSelection,
        .tool = toolIndex,
        .requestDeselect = deselectRequested,
        .requestSelectParent = selectParentRequested,
        .requestFrameSelected = frameSelectedRequested,
        .showGrid = showGrid,
        .showOrigin = showOrigin,
        .showGizmo = showGizmo,
        .showAABBs = showAabbs,
        .showSelectedAABB = showSelectedAabb,
        .showLightGizmos = showLightGizmos,
        .showGuides = showGuides,
        .showProxies = showProxies,
        .depthPrepass = depthPrepass,
        .viewMode = viewMode,
        .debugView = debugView,
        .showBufferOverlay = bufferOverlay,
        .showShadowOverlay = shadowOverlay,
        .antiAliasing = antiAliasing,
        .showFrameGraph = showFrameGraphWindow,
        .showPerformance = showPerformanceWindow,
        .showCvars = showCvarWindow,
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
        PROFILE_ZONE("MenuBar", Ui);
        drawViewMenuBar(menuState);
    }
    if (showStatusBar) {
        PROFILE_ZONE("StatusBar", Ui);
        drawStatusBar(statusBarData);
    }
    {
        PROFILE_ZONE("FrameGraphWindow", Ui);
        std::optional<FrameGraphCaptureRequest> captureRequest;
        drawFrameGraphWindow(showFrameGraphWindow, fgLastSnapshot, fgSelectedPass, fgSelectedResource, captureRequest);
        if (captureRequest.has_value()) {
            captureWindowState.pass = captureRequest->pass;
            captureWindowState.afterLastWrite = false;
            captureWindowState.resource = captureRequest->resource;
            captureWindowState.trigger++;
            introspectionFlags.capture = true;
        }
    }
    {
        PROFILE_ZONE("PerformanceWindow", Ui);
        drawPerformanceWindow(showPerformanceWindow, performanceState);
    }
    {
        PROFILE_ZONE("CvarWindow", Ui);
        drawCvarWindow(showCvarWindow, cvarState);
    }
    {
        PROFILE_ZONE("RenderDebugWindow", Ui);
        RenderDebugViewFlags viewFlags = {
            .viewMode = viewMode,
            .sampler = sampler,
            .shadow = shadow,
            .ambient = ambient,
            .showBufferOverlay = bufferOverlay,
            .showShadowOverlay = shadowOverlay,
            .antiAliasing = antiAliasing,
            .showGrid = showGrid,
            .showOrigin = showOrigin,
            .showGizmo = showGizmo,
            .showAABBs = showAabbs,
            .showLightGizmos = showLightGizmos,
        };
        if (drawRenderDebugWindow(showRenderDebugWindow, renderDebugLast, viewFlags, primPath, renderDebugSelectedPrim, renderDebugDraws)) {
            screenshotRequested = true;
        }
    }
    rendercvars::depthPrepass.set(depthPrepass);
    (void) rendercvars::viewMode.set((size_t) viewMode);
    (void) rendercvars::debugView.set((size_t) debugView);
    rendercvars::bufferOverlay.set(bufferOverlay);
    rendercvars::shadowOverlay.set(shadowOverlay);
    rendercvars::antiAliasing.set(antiAliasing);
    rendercvars::setSamplerSettings(sampler);
    rendercvars::setShadowSettings(shadow);
    rendercvars::setAmbient(ambient);
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
        PROFILE_ZONE("FrameDebuggerWindow", Ui);
        auto openInCapture = [&](const std::string& pass, const std::string& resource) {
            captureWindowState.pass = pass;
            captureWindowState.afterLastWrite = false;
            captureWindowState.resource = resource;
            captureWindowState.trigger++;
            introspectionFlags.capture = true;
        };
        drawFrameDebuggerWindow(introspectionFlags.frameDebugger, frameDebuggerState, instancePath, openInCapture);
    }
    if (introspectionFlags.gpuScene) {
        PROFILE_ZONE("GpuSceneWindow", Ui);
        auto namePrim = [&](uint32_t prim) -> std::string {
            auto path = primPath(prim);
            return !path.empty() ? path : std::string("(unknown prim)");
        };
        // Nothing is selected in the view yet: a click on a row selects nothing.
        auto selectPrim = [](uint32_t) {
        };
        // Every instance's prim, built only while the window is open.
        auto primOfInstance = primOfEachInstance(renderWorld);
        drawGpuSceneWindow(introspectionFlags.gpuScene, gpuSceneState, 1 + cullResult->cascadeCount, primOfInstance, namePrim, selectPrim);
    }
    {
        PROFILE_ZONE("DebugViewWindow", Ui);
        updateDebugViewCursor(debugViewState);
        drawDebugViewWindow(static_cast<DebugView>(rendercvars::debugView.get()), debugViewState, instancePath);
    }
    if (introspectionFlags.remoteView) {
        PROFILE_ZONE("RemoteViewWindow", Ui);
        drawRemoteViewWindow(introspectionFlags.remoteView, remoteViewStatus, remoteControlStatus);
    }
    if (introspectionFlags.counters) {
        PROFILE_ZONE("CountersWindow", Ui);
        drawCountersWindow(introspectionFlags.counters, countersState);
    }
    if (introspectionFlags.capture) {
        PROFILE_ZONE("CaptureWindow", Ui);
        drawCaptureWindow(introspectionFlags.capture, fgLastSnapshot, captureWindowState);
    }
    if (introspectionFlags.memory) {
        PROFILE_ZONE("MemoryWindow", Ui);
        drawMemoryWindow(introspectionFlags.memory, renderDebugLast, memoryWindowState);
    }
    {
        PROFILE_ZONE("CameraWindow", Ui);
        if (tools != nullptr && cameraModes != nullptr) {
            drawCameraWindow(showCameraWindow, camera, cameraState, tools->debugCamera, *cameraModes);
        }
    }
    {
        PROFILE_ZONE("AssetsWindow", Ui);
        drawAssetsWindow(showAssetsWindow, assetLog, assetsState);
    }
    {
        PROFILE_ZONE("LodWindow", Ui);
        if (tools != nullptr) {
            drawLodWindow(showLodWindow, tools->lod, lodWindowState);
        }
    }
    {
        PROFILE_ZONE("StreamingWindow", Ui);
        if (tools != nullptr) {
            drawStreamingWindow(showStreamingWindow, tools->streaming, streamingWindowState);
        }
    }
    {
        PROFILE_ZONE("CullingWindow", Ui);
        // The meshlet culling debug view is a debug view like any other; the window switches it.
        bool meshletCullView = rendercvars::debugView.get() == (size_t) DebugView::MeshletCull;
        bool meshletCullViewBefore = meshletCullView;
        // So is the depth pyramid's view mode, with its level.
        bool pyramidView = rendercvars::viewMode.get() == (size_t) ViewMode::Hzb;
        bool pyramidViewBefore = pyramidView;
        int pyramidLevel = (int) rendercvars::hzbLevel.get();
        drawCullingWindow(showCullingWindow,
                          {.enabled = cullEnabled,
                           .frozen = cullFrozen,
                           .occlusion = occlusion,
                           .showCulled = showCulled,
                           .overlayView = culledView,
                           .showCascadeFrusta = cascadeFrusta,
                           .meshletCullView = meshletCullView,
                           .pyramidView = pyramidView,
                           .pyramidLevel = pyramidLevel,
                           .depthPrepass = depthPrepass,
                           .cull = *cullResult});
        if (meshletCullView != meshletCullViewBefore) {
            (void) rendercvars::debugView.set(meshletCullView ? (size_t) DebugView::MeshletCull : (size_t) DebugView::None);
        }
        if (pyramidView != pyramidViewBefore) {
            (void) rendercvars::viewMode.set(pyramidView ? (size_t) ViewMode::Hzb : (size_t) ViewMode::Lit);
        }
        (void) rendercvars::hzbLevel.set(pyramidLevel);
    }
    toolscvars::overlayGrid.set(showGrid);
    toolscvars::overlayOrigin.set(showOrigin);
    toolscvars::overlayGizmos.set(showGizmo);
    toolscvars::overlayAabbs.set(showAabbs);
    toolscvars::overlaySelectedAabb.set(showSelectedAabb);
    toolscvars::overlayLightGizmos.set(showLightGizmos);
    if (guides != nullptr) {
        guides->set(showGuides);
    }
    if (proxies != nullptr) {
        proxies->set(showProxies);
    }
    rendercvars::cullingEnabled.set(cullEnabled);
    rendercvars::cullingFrozen.set(cullFrozen);
    rendercvars::cullingOcclusion.set(occlusion);
    toolscvars::overlayCulled.set(showCulled);
    (void) toolscvars::overlayCulledView.set(culledView);
    toolscvars::overlayCascadeFrusta.set(cascadeFrusta);
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

auto ViewUI::windowFlag(std::string_view name) -> bool* {
    if (name == "memory") {
        return &introspectionFlags.memory;
    } else if (name == "capture") {
        return &introspectionFlags.capture;
    } else if (name == "framedebugger") {
        return &introspectionFlags.frameDebugger;
    } else if (name == "gpuscene") {
        return &introspectionFlags.gpuScene;
    } else if (name == "counters") {
        return &introspectionFlags.counters;
    } else if (name == "shaders") {
        return &introspectionFlags.shaders;
    } else if (name == "remoteview") {
        return &introspectionFlags.remoteView;
    } else if (name == "culling") {
        return &showCullingWindow;
    } else if (name == "assets") {
        return &showAssetsWindow;
    } else if (name == "lod") {
        return &showLodWindow;
    } else if (name == "streaming") {
        return &showStreamingWindow;
    } else if (name == "camera") {
        return &showCameraWindow;
    } else if (name == "renderdebug") {
        return &showRenderDebugWindow;
    } else if (name == "framegraph") {
        return &showFrameGraphWindow;
    } else if (name == "performance") {
        return &showPerformanceWindow;
    } else if (name == "cvars") {
        return &showCvarWindow;
    }
    return nullptr;
}

auto ViewUI::setIntrospectionWindow(std::string_view name, bool on) -> bool {
    auto* open = windowFlag(name);
    if (open == nullptr) {
        return false;
    }
    *open = on;
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
    if (auto watch = debugViewReadoutWatchFor(debugViewState, static_cast<DebugView>(rendercvars::debugView.get())); watch.has_value()) {
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

auto ViewUI::addInputWaiter(std::function<void(const InputWants&)> waiter) -> void {
    inputWaiters.push_back(std::move(waiter));
}

auto ViewUI::inputWants() const -> InputWants {
    const auto& io = ImGui::GetIO();
    return {
        .mouse = io.WantCaptureMouse,
        .keyboard = io.WantCaptureKeyboard,
        .text = io.WantTextInput,
        .popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel),
    };
}

auto ViewUI::answerInputWaiters() -> void {
    if (inputWaiters.empty()) {
        return;
    }
    auto wants = inputWants();
    auto waiters = std::move(inputWaiters);
    inputWaiters.clear();
    for (auto& waiter : waiters) {
        waiter(wants);
    }
}
