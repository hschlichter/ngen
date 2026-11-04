#include "toolshooks.h"

#include "overlays.h"
#include "primpath.h"
#include "profile.h"
#include "projection.h"
#include "renderconfig.h"
#include "renderer.h"
#include "viewframe.h"

#include <SDL3/SDL_events.h>

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

auto ToolsHooks::afterScene(ViewFrame& frame) -> void {
    // The LOD tool's rows (the LOD window and its viewport layers), once per snapshot while either is shown.
    lodShown = tools.lod.windowOpen || toolsconfig::lodLabels.get() || toolsconfig::lodGizmo.get() || toolsconfig::lodGizmoAll.get();
    if (lodShown && lodRowsSnapshot != frame.snapshotsTaken) {
        lodRowsSnapshot = frame.snapshotsTaken;
        PROFILE_ZONE("SceneWindowLists");
        buildLodRows(frame.scene, tools.lod);
    }
    // The Streaming tool's rows (its window, its viewport layers and the streaming view's states), once per snapshot while any is shown.
    streamingView = renderconfig::debugView.get() == (size_t) DebugView::Streaming;
    streamingShown = tools.streaming.windowOpen || toolsconfig::streamingBounds.get() || toolsconfig::streamingRings.get() || toolsconfig::streamingRingsAll.get() ||
                     toolsconfig::streamingLabels.get() || streamingView;
    if (streamingShown && streamingRowsSnapshot != frame.snapshotsTaken) {
        streamingRowsSnapshot = frame.snapshotsTaken;
        PROFILE_ZONE("SceneWindowLists");
        buildStreamingRows(frame.scene, tools.streaming);
    }
}

auto ToolsHooks::cameras(ViewFrame& frame) -> void {
    // The debug camera, placed behind the main camera when it is turned on or asked to be; the main camera's trail starts afresh.
    auto& debug = tools.debugCamera;
    bool on = toolsconfig::debugCamera.get();
    if (on && (!debugCameraWasOn || debug.placeRequested)) {
        placeDebugCamera(debug, frame.cam, frame.scene.lodMetersPerUnit());
        if (!debugCameraWasOn) {
            debug.trailPoints.clear();
        }
    }
    debug.placeRequested = false;
    debugCameraWasOn = on;
    debug.camera.worldUp = frame.cam.worldUp;
    if (!on) {
        return;
    }
    // While on, the view renders from it and the keys move it, unless they are handed to the main camera; LOD and streaming, culling and
    // the shadow cascades follow it when set to.
    frame.cameras.view = &debug.camera;
    if (!toolsconfig::debugControlMain.get()) {
        frame.cameras.controlled = &debug.camera;
    }
    frame.cameras.lod = &lodCamera(debug, frame.cam);
    if (toolsconfig::debugCull.get()) {
        frame.cameras.cull = &debug.camera;
    }
    if (toolsconfig::debugShadows.get()) {
        frame.cameras.shadows = &debug.camera;
    }
}

auto ToolsHooks::event(ViewFrame& frame, const SDL_Event& event) -> bool {
    // C hands the controls between the debug camera and the main camera while the debug camera is on.
    bool plainKey = event.type == SDL_EVENT_KEY_DOWN && (event.key.mod & SDL_KMOD_CTRL) == 0 && !event.key.repeat;
    if (plainKey && event.key.key == SDLK_C && toolsconfig::debugCamera.get()) {
        toolsconfig::debugControlMain.set(!toolsconfig::debugControlMain.get());
        return true;
    }

    // A gizmo drag in progress: the new local transform is the view's own opinion on the prim, until the view closes.
    auto extent = RhiExtent2D{.width = (uint32_t) frame.width, .height = (uint32_t) frame.height};
    auto view = frame.cameras.view->viewMatrix();
    bool dragging = translateGizmo.isDragging() || rotateGizmo.isDragging() || scaleGizmo.isDragging();
    auto dragTo = [&](float x, float y) -> std::optional<Transform> {
        if (translateGizmo.isDragging()) {
            return translateGizmo.dragUpdate(x, y, extent, view, frame.proj);
        }
        if (rotateGizmo.isDragging()) {
            return rotateGizmo.dragUpdate(x, y, extent, view, frame.proj);
        }
        return scaleGizmo.dragUpdate(x, y, extent, view, frame.proj);
    };
    if (event.type == SDL_EVENT_MOUSE_MOTION && dragging) {
        if (auto local = dragTo(event.motion.x, event.motion.y)) {
            frame.scene.setLocalTransform(frame.selectedPath, local->toMat4());
        }
        return true;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT && dragging) {
        if (auto local = dragTo(event.button.x, event.button.y)) {
            frame.scene.setLocalTransform(frame.selectedPath, local->toMat4());
        }
        translateGizmo.release();
        rotateGizmo.release();
        scaleGizmo.release();
        return true;
    }

    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN || event.button.button != SDL_BUTTON_LEFT) {
        return false;
    }
    float mx = event.button.x;
    float my = event.button.y;
    // The axis gizmo turns the camera rendered from to look along an axis, the way it already faces.
    if (auto axis = frame.renderer.gizmoHitTest(mx, my)) {
        auto& camera = *frame.cameras.view;
        camera.snapToAxis(*axis, camera.forward()[*axis] < 0.0f);
        return true;
    }
    // The active tool's gizmo grabs the selection when it takes a transform.
    auto xf = frame.scene.transform(frame.selectedPath);
    if (!xf || !xf->xformable || frame.selectedPath.empty()) {
        return false;
    }
    auto local = decomposeTransform(xf->local);
    auto anchor = gizmoAnchor(frame, *xf);
    switch (tools.activeTool) {
        case ViewTool::Translate:
            return translateGizmo.tryGrab(mx, my, extent, view, frame.proj, anchor, local, xf->world);
        case ViewTool::Rotate:
            return rotateGizmo.tryGrab(mx, my, extent, view, frame.proj, anchor, local, xf->world);
        case ViewTool::Scale:
            return scaleGizmo.tryGrab(mx, my, extent, view, frame.proj, anchor, local, xf->world);
        case ViewTool::Select:
            return false;
    }
    return false;
}

auto ToolsHooks::afterCamera(ViewFrame& frame) -> void {
    if (toolsconfig::debugCamera.get()) {
        extendDebugCameraTrail(tools.debugCamera, frame.cam.position, frame.scene.lodMetersPerUnit());
    }
    // While the LOD tool is shown, the frames read every render-LOD group's state back, and its rows take them.
    frame.readLodStates = frame.readLodStates || lodShown;
    if (lodShown) {
        updateLodRows(frame.scene, frame.latestCull, tools.lod);
    }
}

auto ToolsHooks::debugDraw(ViewFrame& frame) -> void {
    // The culled overlay shows one view's GPU culling result, the camera or a cascade, from the visibility bits it has read back while on.
    std::vector<uint8_t> overlayVisible;
    if (toolsconfig::overlayCulled.get()) {
        PROFILE_ZONE("CullOverlay");
        const auto& cull = frame.latestCull;
        overlayVisible.resize(cull.viewBits.size());
        auto overlayBit = (uint8_t) (1u << std::clamp((int) toolsconfig::overlayCulledView.get(), 0, (int) maxShadowCascades));
        for (size_t i = 0; i < overlayVisible.size(); i++) {
            overlayVisible[i] = (cull.viewBits[i] & overlayBit) != 0 ? 1 : 0;
        }
    }
    const auto& selectedPath = frame.selectedPath;
    if (selectedPath != selectedBoundsPath || &frame.scene.snapshot() != selectedBoundsSnapshot) {
        selectedBoundsPath = selectedPath;
        selectedBoundsSnapshot = &frame.scene.snapshot();
        selectedBounds = selectedPath.empty() ? AABB{} : frame.scene.bounds(selectedPath);
    }
    PROFILE_ZONE("DrawDebug");
    auto& draw = frame.debugDraw;
    auto up = frame.cam.worldUp;
    auto metersPerUnit = frame.scene.lodMetersPerUnit();
    drawOverlays(draw,
                 frame.scene.renderWorld(),
                 frame.cameras.view->position,
                 up,
                 overlayVisible,
                 frame.frozenCorners,
                 std::span<const ShadowCascade>(frame.cascades.data(), frame.cascadeCount),
                 selectedPath.empty() ? nullptr : &selectedBounds);
    drawLodGizmos(tools.lod, draw, up, lodWindowArea(glm::radians(cameraFovDegrees), frame.viewAspect));
    drawStreamingOverlays(tools.streaming, draw, up);
    if (toolsconfig::debugCamera.get()) {
        drawMainCamera(tools.debugCamera, frame.cam, frame.proj * frame.cam.viewMatrix(), projectionDepthAt((float) renderconfig::shadowDistance.get()), metersPerUnit, draw);
    }
}

auto ToolsHooks::drawUi(ViewFrame& frame) -> void {
    syncLodTool(frame);
    syncStreamingTool(frame);
}

auto ToolsHooks::syncLodTool(ViewFrame& frame) -> void {
    PROFILE_ZONE("LodWindowSync");
    auto& scene = frame.scene;
    auto& lod = tools.lod;
    const auto& cull = frame.latestCull;
    // The levels the window asked for, and the root it selects or frames; it highlights the root at or above the selection.
    for (const auto& request : std::exchange(lod.levelRequests, {})) {
        if (request.level) {
            scene.setLodLevel(request.root, *request.level);
        } else {
            scene.resetLodLevel(request.root);
        }
    }
    // Render LOD: the GPU's counts.
    lod.renderLod.groups = scene.threadStats().lodGroups;
    lod.renderLod.drawing = cull.lodPicked;
    lod.renderLod.fallbacks = cull.lodFallbacks;
    lod.renderLod.forced = cull.lodForced;
    lod.renderLod.fading = cull.lodFading;
    lod.renderLod.popped = cull.lodPopped;
    lod.renderLod.pops = frame.lodPops;
    lod.renderLod.slotsNotDrawn = cull.lodDropped;
    // The tuning, and what the window asked to change.
    const auto& tuning = scene.lodTuning();
    lod.bias = tuning.bias;
    lod.hysteresis = tuning.hysteresis;
    lod.blending = tuning.blending;
    lod.overrides = (uint32_t) tuning.overrides.size();
    auto bias = std::exchange(lod.biasRequested, std::nullopt);
    auto hysteresis = std::exchange(lod.hysteresisRequested, std::nullopt);
    auto blending = std::exchange(lod.blendingRequested, std::nullopt);
    if (bias || hysteresis || blending) {
        scene.setLodTuning(bias, hysteresis, blending);
    }
    for (auto& request : std::exchange(lod.overrideRequests, {})) {
        scene.overrideLodThresholds(request.root, request.heuristic, LodOverride{.thresholds = std::move(request.thresholds), .blendThresholds = std::move(request.blendThresholds)});
    }
    for (const auto& root : std::exchange(lod.overrideResets, {})) {
        scene.resetLodOverrides(root);
    }
    // The viewport's layers: the LOD Level view and the Triangle Size debug view are the view's modes.
    lod.levelView = renderconfig::bufferView.get() == (size_t) GBufferView::LodLevel && renderconfig::debugView.get() == 0;
    if (auto on = std::exchange(lod.levelViewRequested, std::nullopt)) {
        (void) renderconfig::bufferView.set((size_t) (*on ? GBufferView::LodLevel : GBufferView::Lit));
        if (*on) {
            (void) renderconfig::debugView.set((size_t) DebugView::None);
        }
    }
    if (std::exchange(lod.triangleSizeRequested, std::nullopt)) {
        (void) renderconfig::debugView.set((size_t) DebugView::TriangleSize);
    }
    lod.heuristic = scene.lodHeuristic();
    lod.preference = (int) scene.lodPreference();
    if (auto preference = std::exchange(lod.preferenceRequested, std::nullopt)) {
        scene.setLodPreference((LodPreference) *preference);
    }
    if (auto on = std::exchange(lod.heuristicRequested, std::nullopt)) {
        scene.setLodHeuristic(*on);
    }
    if (auto path = std::exchange(lod.selectRequested, std::nullopt)) {
        frame.selectedPath = *path;
    }
    if (auto path = std::exchange(lod.frameRequested, std::nullopt)) {
        auto bb = scene.bounds(*path);
        if (bb.valid()) {
            frame.cam.frame(bb, glm::radians(45.0f));
        }
    }
    const auto& selectedPath = frame.selectedPath;
    lod.selectedRoot.clear();
    lod.selectedPath = selectedPath;
    if (!selectedPath.empty()) {
        for (const auto& row : lod.rows) {
            bool atOrAbove = primpath::isAtOrUnder(selectedPath, row.path);
            if (atOrAbove && row.path.size() > lod.selectedRoot.size()) {
                lod.selectedRoot = row.path;
            }
        }
    }
    // The inspector: the selected root's, again when the selection or the snapshot changes and four times a second while shown (its
    // measures follow the camera).
    auto now = std::chrono::steady_clock::now();
    if (lod.windowOpen && (lod.selectedRoot != lodInspected || frame.snapshotsTaken != lodInspectedSnapshot || now - lodInspectedAt > std::chrono::milliseconds(250))) {
        lodInspected = lod.selectedRoot;
        lodInspectedSnapshot = frame.snapshotsTaken;
        lodInspectedAt = now;
        lod.inspection = inspectLodRoot(scene, cull, lod.selectedRoot, {.eye = frame.cameras.lod->position, .fovY = glm::radians(cameraFovDegrees), .aspect = frame.viewAspect});
    }
}

auto ToolsHooks::syncStreamingTool(ViewFrame& frame) -> void {
    PROFILE_ZONE("StreamingWindowSync");
    auto& scene = frame.scene;
    auto& streaming = tools.streaming;
    // A sample for the chart, the cameras for the map, the flight for the controls.
    const auto& threadStats = scene.threadStats();
    streaming.totals.inFlight = threadStats.inFlightLayers + threadStats.inFlightMeshes + threadStats.inFlightMaterials + threadStats.inFlightTextures + threadStats.inFlightOther;
    if (streaming.windowOpen) {
        auto at = streaming.historyAt;
        streaming.inFlightHistory[at] = (float) streaming.totals.inFlight;
        streaming.lateHistory[at] = (float) scene.streaming().lateNow;
        streaming.holeHistory[at] = (float) scene.streaming().holesNow;
        streaming.historyAt = (at + 1) % StreamingToolState::historyLength;
    }
    const auto& lodCam = *frame.cameras.lod;
    streaming.cameraPosition = lodCam.position;
    streaming.debugCameraPosition = toolsconfig::debugCamera.get() ? std::optional<glm::vec3>(tools.debugCamera.camera.position) : std::nullopt;
    streaming.cameraForward = lodCam.forward();
    streaming.up = frame.cam.worldUp;
    const auto& flight = frame.flight;
    streaming.flying = flight.flying();
    if (flight.flying()) {
        streaming.flightText = std::format("{} of {} cameras, {:.0f} m in {:.0f} s", flight.segment + 1, flight.cameras.size(), flight.metres, flight.seconds);
    }
    streaming.streamingView = streamingView;
    // What the window asked for.
    if (auto on = std::exchange(streaming.streamingViewRequested, std::nullopt)) {
        (void) renderconfig::debugView.set((size_t) (*on ? DebugView::Streaming : DebugView::None));
    }
    if (std::exchange(streaming.resetRequested, false)) {
        scene.resetStreamingCounters();
    }
    if (std::exchange(streaming.flyRequested, false)) {
        if (auto error = startFlight(frame.flight, frame.cam, scene, streaming.flightPrefix, std::max(0.1f, streaming.flightSpeed), nullptr)) {
            streaming.flightText = *error;
        }
    }
    if (std::exchange(streaming.flyStopRequested, false)) {
        stopFlight(frame.flight);
    }
    for (auto& request : std::exchange(streaming.radiiRequests, {})) {
        scene.overrideLodThresholds(request.root, request.heuristic, LodOverride{.thresholds = std::move(request.load), .unloadThresholds = std::move(request.unload)});
    }
    for (const auto& root : std::exchange(streaming.radiiResets, {})) {
        scene.resetLodOverrides(root);
    }
    if (auto path = std::exchange(streaming.selectRequested, std::nullopt)) {
        frame.selectedPath = *path;
    }
    if (auto path = std::exchange(streaming.frameRequested, std::nullopt)) {
        auto bb = scene.bounds(*path);
        if (bb.valid()) {
            frame.cam.frame(bb, glm::radians(45.0f));
        }
    }
    const auto& selectedPath = frame.selectedPath;
    streaming.selectedPath = selectedPath;
    streaming.selectedRoot.clear();
    if (!selectedPath.empty()) {
        for (const auto& row : streaming.rows) {
            bool atOrAbove = primpath::isAtOrUnder(selectedPath, row.path);
            if (atOrAbove && row.path.size() > streaming.selectedRoot.size()) {
                streaming.selectedRoot = row.path;
            }
        }
    }
    // The inspector: the selected root's, again when the selection or the snapshot changes and four times a second while shown.
    auto now = std::chrono::steady_clock::now();
    if (streaming.windowOpen &&
        (streaming.selectedRoot != streamingInspected || frame.snapshotsTaken != streamingInspectedSnapshot || now - streamingInspectedAt > std::chrono::milliseconds(250))) {
        streamingInspected = streaming.selectedRoot;
        streamingInspectedSnapshot = frame.snapshotsTaken;
        streamingInspectedAt = now;
        streaming.inspection = inspectStreamingRoot(scene, streaming.selectedRoot, lodCam.position, lodWindowArea(glm::radians(cameraFovDegrees), frame.viewAspect));
    }
}

auto ToolsHooks::fillSnapshot(ViewFrame& frame, RenderSnapshot& snapshot) -> void {
    // The active tool's gizmo, on the selection when it takes a transform.
    if (frame.selectedPath != selectedTransformPath || &frame.scene.snapshot() != selectedTransformSnapshot) {
        PROFILE_ZONE("SelectionTransform");
        selectedTransformPath = frame.selectedPath;
        selectedTransformSnapshot = &frame.scene.snapshot();
        selectedTransform = frame.selectedPath.empty() ? std::nullopt : frame.scene.transform(frame.selectedPath);
    }
    bool shown = selectedTransform && selectedTransform->xformable;
    auto anchor = shown ? gizmoAnchor(frame, *selectedTransform) : glm::vec3(0.0f);
    {
        PROFILE_ZONE("Gizmos");
        auto extent = RhiExtent2D{.width = (uint32_t) frame.width, .height = (uint32_t) frame.height};
        const auto& camera = *frame.cameras.view;
        auto view = camera.viewMatrix();
        auto tool = tools.activeTool;
        translateGizmo.update(extent, view, frame.proj, camera.position, frame.mouse.x, frame.mouse.y, shown && tool == ViewTool::Translate, anchor);
        rotateGizmo.update(extent, view, frame.proj, camera.position, frame.mouse.x, frame.mouse.y, shown && tool == ViewTool::Rotate, anchor);
        scaleGizmo.update(extent, view, frame.proj, camera.position, frame.mouse.x, frame.mouse.y, shown && tool == ViewTool::Scale, anchor);
    }
    snapshot.showGizmo = toolsconfig::overlayGizmos.get();
    snapshot.readVisibility = toolsconfig::overlayCulled.get();
    snapshot.streamingStates = streamingView ? tools.streaming.slotStates : std::vector<uint32_t>();
    snapshot.lodPopFlash = toolsconfig::lodPopFlash.get();
    snapshot.lodColorByDepth = toolsconfig::lodColorByDepth.get();
    snapshot.translateGizmoVerts = {translateGizmo.vertices().begin(), translateGizmo.vertices().end()};
    snapshot.rotateGizmoVerts = {rotateGizmo.vertices().begin(), rotateGizmo.vertices().end()};
    snapshot.scaleGizmoVerts = {scaleGizmo.vertices().begin(), scaleGizmo.vertices().end()};
}

auto ToolsHooks::gizmoAnchor(const ViewFrame& frame, const ViewScene::PrimTransform& transform) const -> glm::vec3 {
    auto anchor = glm::vec3(transform.world[3]);
    auto bb = frame.scene.bounds(frame.selectedPath);
    if (bb.valid() && !bb.contains(anchor)) {
        anchor = (bb.min + bb.max) * 0.5f;
    }
    return anchor;
}
