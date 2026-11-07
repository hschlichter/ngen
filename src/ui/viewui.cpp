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
        if (auto* debugCamera = configBool("tools.debugcamera.on")) {
            debugCamera->set(!debugCamera->get());
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
    bool showGrid = toolsconfig::overlayGrid.get();
    bool showOrigin = toolsconfig::overlayOrigin.get();
    bool showGizmo = toolsconfig::overlayGizmos.get();
    bool showAabbs = toolsconfig::overlayAabbs.get();
    bool showSelectedAabb = toolsconfig::overlaySelectedAabb.get();
    bool showLightGizmos = toolsconfig::overlayLightGizmos.get();
    auto* guides = configBool("scene.purposes.guide");
    auto* proxies = configBool("scene.purposes.proxy");
    bool showGuides = guides != nullptr && guides->get();
    bool showProxies = proxies != nullptr && proxies->get();
    bool cullEnabled = renderconfig::cullingEnabled.get();
    bool cullFrozen = renderconfig::cullingFrozen.get();
    bool occlusion = renderconfig::cullingOcclusion.get();
    bool showCulled = toolsconfig::overlayCulled.get();
    int culledView = (int) toolsconfig::overlayCulledView.get();
    bool cascadeFrusta = toolsconfig::overlayCascadeFrusta.get();
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
        .showGrid = showGrid,
        .showOrigin = showOrigin,
        .showGizmo = showGizmo,
        .showAABBs = showAabbs,
        .showSelectedAABB = showSelectedAabb,
        .showLightGizmos = showLightGizmos,
        .showGuides = showGuides,
        .showProxies = showProxies,
        .depthPrepass = depthPrepass,
        .gbufferView = gbufferView,
        .debugView = debugView,
        .showBufferOverlay = bufferOverlay,
        .showShadowOverlay = shadowOverlay,
        .antiAliasing = antiAliasing,
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
            captureWindowState.afterLastWrite = false;
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
            captureWindowState.afterLastWrite = false;
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
        drawGpuSceneWindow(introspectionFlags.gpuScene, gpuSceneState, 1 + cullResult->cascadeCount, primOfInstance, namePrim, selectPrim);
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
        if (tools != nullptr && cameraModes != nullptr) {
            drawCameraWindow(showCameraWindow, camera, cameraState, tools->debugCamera, *cameraModes);
        }
    }
    {
        PROFILE_ZONE("AssetsWindow");
        drawAssetsWindow(showAssetsWindow, assetLog, assetsState);
    }
    {
        PROFILE_ZONE("LodWindow");
        if (tools != nullptr) {
            drawLodWindow(showLodWindow, tools->lod, lodWindowState);
        }
    }
    {
        PROFILE_ZONE("StreamingWindow");
        if (tools != nullptr) {
            drawStreamingWindow(showStreamingWindow, tools->streaming, streamingWindowState);
        }
    }
    {
        PROFILE_ZONE("CullingWindow");
        // The meshlet culling debug view is a debug view like any other; the window switches it.
        bool meshletCullView = renderconfig::debugView.get() == (size_t) DebugView::MeshletCull;
        bool meshletCullViewBefore = meshletCullView;
        drawCullingWindow(showCullingWindow,
                          {.enabled = cullEnabled,
                           .frozen = cullFrozen,
                           .occlusion = occlusion,
                           .showCulled = showCulled,
                           .overlayView = culledView,
                           .showCascadeFrusta = cascadeFrusta,
                           .meshletCullView = meshletCullView,
                           .depthPrepass = depthPrepass,
                           .cull = *cullResult});
        if (meshletCullView != meshletCullViewBefore) {
            (void) renderconfig::debugView.set(meshletCullView ? (size_t) DebugView::MeshletCull : (size_t) DebugView::None);
        }
    }
    toolsconfig::overlayGrid.set(showGrid);
    toolsconfig::overlayOrigin.set(showOrigin);
    toolsconfig::overlayGizmos.set(showGizmo);
    toolsconfig::overlayAabbs.set(showAabbs);
    toolsconfig::overlaySelectedAabb.set(showSelectedAabb);
    toolsconfig::overlayLightGizmos.set(showLightGizmos);
    if (guides != nullptr) {
        guides->set(showGuides);
    }
    if (proxies != nullptr) {
        proxies->set(showProxies);
    }
    renderconfig::cullingEnabled.set(cullEnabled);
    renderconfig::cullingFrozen.set(cullFrozen);
    renderconfig::cullingOcclusion.set(occlusion);
    toolsconfig::overlayCulled.set(showCulled);
    (void) toolsconfig::overlayCulledView.set(culledView);
    toolsconfig::overlayCascadeFrusta.set(cascadeFrusta);
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
    } else if (name == "config") {
        return &showConfigWindow;
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
