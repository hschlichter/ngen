#include "viewframe.h"

#include "assetclient.h"
#include "profile.h"
#include "profileconfig.h"
#include "projection.h"
#include "renderconfig.h"
#include "renderthread.h"
#include "trace.h"
#include "viewscene.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <limits>
#include <utility>

ViewFrame::ViewFrame(ViewScene& scene, Renderer& renderer, RenderThread& renderThread, AssetClient& assets)
    : scene(scene), renderer(renderer), renderThread(renderThread), assets(assets), cam{
                                                                                        .position = glm::vec3(2.0f, 1.5f, 2.0f),
                                                                                        .worldUp = scene.worldUp(),
                                                                                        .yaw = -135.0f,
                                                                                        .pitch = -20.0f,
                                                                                        .speed = 5.0f,
                                                                                        .mouseSensitivity = 0.15f,
                                                                                    }
    , postedCameraPosition(std::numeric_limits<float>::quiet_NaN())
    , uploadBudgetChanges(renderconfig::uploadBudgetMiB.changes())
    , geometryLimitChanges(renderconfig::geometryLimitMiB.changes()) {
    resetCameras();
}

auto ViewFrame::updateScene() -> void {
    frameCounter++;
    // What the asset server said changed at its source: shaders reload on the render thread, the scene's assets are fetched again and the
    // scene rebuilt (blocking, as a repack is).
    if (auto changed = assets.takeChanged(); !changed.empty()) {
        renderThread.reloadShaders(changed);
        scene.refresh(changed, false);
    }
    static const uint32_t sceneUpdateZoneId = profile::registerSite("SceneUpdate", profile::Group::Frame);
    profile::beginZone(sceneUpdateZoneId);
    bool sceneUpdated = scene.update();
    profile::endZone();
    if (sceneUpdated) {
        PROFILE_ZONE("SceneInputs", Frame);
        snapshotsTaken++;
        cam.worldUp = scene.worldUp();
        placeStartCamera();
        if (autoFrame) {
            frameArrived();
        }
    }
    poseBeforeCommands = cam.position;
    yawBeforeCommands = cam.yaw;
    pitchBeforeCommands = cam.pitch;
}

auto ViewFrame::advanceTime(uint64_t nowNs) -> void {
    PROFILE_ZONE_DETAIL("AdvanceTime", Frame);
    // A script or an RPC call that moves the camera ends the following, as a hand on the controls does.
    if (cam.position != poseBeforeCommands || cam.yaw != yawBeforeCommands || cam.pitch != pitchBeforeCommands) {
        autoFrame = false;
    }
    if (lastTicks == 0) {
        lastTicks = nowNs;
    }
    dt = (float) (nowNs - lastTicks) / 1.0e9f;
    lastTicks = nowNs;
    if (!residency.resident && frameCounter > 1) {
        residency.longestLoadingFrameMs = std::max(residency.longestLoadingFrameMs, (double) dt * 1000.0);
        residency.loadingFrames++;
    }
    if (flight.flying()) {
        advanceFlight(flight, cam, dt);
        autoFrame = false;
    }
}

auto ViewFrame::resetCameras() -> void {
    PROFILE_ZONE_DETAIL("ResetCameras", Frame);
    cameras = {
        .view = &cam,
        .controlled = &cam,
        .lod = &cam,
        .cull = &cam,
        .shadows = &cam,
    };
}

auto ViewFrame::setViewport(int newWidth, int newHeight) -> void {
    width = newWidth;
    height = newHeight;
    proj = perspectiveProjection(glm::radians(cameraFovDegrees), (float) width / (float) height);
    viewAspect = (float) width / (float) std::max(1, height);
}

auto ViewFrame::sendSceneInputs() -> void {
    PROFILE_ZONE_DETAIL("SendSceneInputs", Frame);
    // The camera for the LOD heuristic, when it moved: once it has been placed (by hand, a script, or the framing when the scene was first
    // resident), not while it follows what has arrived.
    bool cameraPlaced = residency.everResident || !autoFrame;
    const auto& lodCam = *cameras.lod;
    if (cameraPlaced && (lodCam.position != postedCameraPosition || width != postedCameraWidth || height != postedCameraHeight)) {
        postedCameraPosition = lodCam.position;
        postedCameraWidth = width;
        postedCameraHeight = height;
        scene.setCamera(lodCam.position, glm::radians(cameraFovDegrees), viewAspect);
    }

    // The purposes the View menu or a script turned on or off: the scene loads and draws them, and moves its revision on.
    if (scene.purposeShown(Purpose::Guide) != viewconfig::purposeGuide.get()) {
        scene.setPurposeShown(Purpose::Guide, viewconfig::purposeGuide.get());
    }
    if (scene.purposeShown(Purpose::Proxy) != viewconfig::purposeProxy.get()) {
        scene.setPurposeShown(Purpose::Proxy, viewconfig::purposeProxy.get());
    }
    // The upload budget and the geometry limit, handed to the render thread when their configuration changes.
    if (renderconfig::uploadBudgetMiB.changes() != uploadBudgetChanges) {
        uploadBudgetChanges = renderconfig::uploadBudgetMiB.changes();
        renderThread.setUploadBudget((uint64_t) renderconfig::uploadBudgetMiB.get() * 1024 * 1024);
    }
    if (renderconfig::geometryLimitMiB.changes() != geometryLimitChanges) {
        geometryLimitChanges = renderconfig::geometryLimitMiB.changes();
        renderThread.setGeometryLimit((uint64_t) renderconfig::geometryLimitMiB.get() * 1024 * 1024);
    }
    // The profiler's history window and GPU detail: the collector reads them from any thread.
    if (profileconfig::historySeconds.changes() != profileHistoryChanges) {
        profileHistoryChanges = profileconfig::historySeconds.changes();
        profile::setHistorySeconds(profileconfig::historySeconds.get());
    }
    if (profileconfig::gpuDetail.changes() != profileGpuDetailChanges) {
        profileGpuDetailChanges = profileconfig::gpuDetail.changes();
        profile::setGpuDetail(profileconfig::gpuDetail.get());
    }
    // What changed in the scene since the last frame: the render thread applies it, uploads go out a budget per frame.
    PROFILE_ZONE("TakeChanges", Frame);
    if (auto changes = scene.takeChanges(); !changes.empty()) {
        groundMeshes.apply(changes);
        renderThread.submitSceneChanges(std::move(changes));
    }
}

auto ViewFrame::updateResidency() -> void {
    static const uint32_t residencyZoneId = profile::registerSite("Residency", profile::Group::Frame);
    profile::beginZone(residencyZoneId);
    bool resident = scene.idle() && renderThread.sceneDrawn();
    if (resident && !residency.resident) {
        if (!residency.everResident) {
            residency.everResident = true;
            if (!startCamera.empty()) {
                TRACE_WARNING("Scene", "StartCameraNotFound", startCamera)
                    .text(std::format("the scene authors no camera at {}; the view starts framing the scene", startCamera));
                startCamera.clear();
            }
            if (autoFrame) {
                frameArrived();
            }
            autoFrame = false;
        }
        auto stats = assets.stats();
        TRACE_EVENT("Scene", "SceneResident", scene.sceneId().empty() ? "(empty scene)" : scene.sceneId())
            .text(std::format("scene resident at frame {}, {:.0f} ms after start: {} draws, {} assets received; longest of the {} frames before, {:.1f} ms",
                              frameCounter,
                              trace::msSinceStart(),
                              std::ranges::count_if(scene.renderWorld().meshInstances,
                                                    [](const RenderMeshInstance& instance) {
                                                        return instance.alive;
                                                    }),
                              stats.received,
                              residency.loadingFrames,
                              residency.longestLoadingFrameMs))
            .field("frame", (int64_t) frameCounter)
            .field("since_start_ms", trace::msSinceStart())
            .field("assets", (int64_t) stats.received)
            .field("longest_frame_ms", residency.longestLoadingFrameMs);
        residency.lastResidentFrame = frameCounter;
        residency.lastResidentMs = trace::msSinceStart();
        residency.lastLoadFrames = residency.loadingFrames;
        residency.lastLoadLongestMs = residency.longestLoadingFrameMs;
        residency.longestLoadingFrameMs = 0.0;
        residency.loadingFrames = 0;
    }
    residency.resident = resident;
    profile::endZone();
}

auto ViewFrame::look(float dx, float dy) -> void {
    auto& camera = *cameras.controlled;
    if (cameraModes.orbiting) {
        camera.orbit(cameraModes.pivot, dx * camera.mouseSensitivity, -dy * camera.mouseSensitivity);
    } else {
        camera.handleMouseMotion(dx, dy);
    }
    autoFrame = false;
}

auto ViewFrame::wheel(float steps) -> void {
    if (cameraModes.orbiting) {
        cameras.controlled->dolly(cameraModes.pivot, steps);
    } else {
        cameras.controlled->scaleSpeed(steps);
    }
}

auto ViewFrame::startOrbit(glm::uvec2 pixel) -> void {
    // The world point at a pixel of a framebuffer `size` and a depth, through an inverse view-projection.
    auto unproject = [](glm::uvec2 at, glm::uvec2 size, float depth, const glm::mat4& inverseViewProj) -> glm::vec3 {
        auto ndc = glm::vec4((((float) at.x + 0.5f) / (float) size.x * 2.0f) - 1.0f, (((float) at.y + 0.5f) / (float) size.y * 2.0f) - 1.0f, depth, 1.0f);
        auto world = inverseViewProj * ndc;
        return glm::vec3(world) / world.w;
    };
    auto inverseViewProj = glm::inverse(proj * cameras.view->viewMatrix());
    auto eye = cameras.view->position;
    auto unitsPerMetre = (float) (1.0 / scene.lodMetersPerUnit());
    auto size = glm::uvec2((uint32_t) width, (uint32_t) height);
    auto direction = glm::normalize(unproject(pixel, size, 1.0f, inverseViewProj) - eye);
    cameraModes.orbiting = true;
    cameraModes.pivotPicked = false;
    cameraModes.pivot = eye + (direction * (cameraModes.pivotDistanceMetres * unitsPerMetre));
    picking.requestDepth(pixel.x, pixel.y, [this, unproject, pixel, size, inverseViewProj, eye](std::optional<float> depth) {
        if (!depth || !cameraModes.orbiting || cameraModes.pivotPicked) {
            return;
        }
        cameraModes.pivot = unproject(pixel, size, *depth, inverseViewProj);
        cameraModes.pivotPicked = true;
        cameraModes.pivotDistanceMetres = (float) (glm::distance(eye, cameraModes.pivot) * scene.lodMetersPerUnit());
    });
}

auto ViewFrame::pick(glm::uvec2 pixel) -> void {
    picking.request(pixel.x, pixel.y, [this](std::optional<uint32_t> instance) {
        selectedPath = instance ? scene.instancePath(*instance) : std::string();
    });
}

auto ViewFrame::deselect() -> void {
    selectedPath.clear();
}

auto ViewFrame::selectParent() -> void {
    auto slash = selectedPath.rfind('/');
    selectedPath = slash == 0 || slash == std::string::npos ? std::string() : selectedPath.substr(0, slash);
}

auto ViewFrame::frameSelected() -> void {
    auto bb = scene.bounds(selectedPath);
    if (!selectedPath.empty() && bb.valid()) {
        cameras.controlled->frame(bb, glm::radians(45.0f));
    }
}

auto ViewFrame::frameScene() -> void {
    // The union of every drawn instance's world bounds, so the first view works for any up axis and asset scale.
    AABB bounds = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
    for (const auto& inst : scene.renderWorld().meshInstances) {
        if (inst.alive && inst.worldBounds.valid()) {
            bounds.min = glm::min(bounds.min, inst.worldBounds.min);
            bounds.max = glm::max(bounds.max, inst.worldBounds.max);
        }
    }
    if (bounds.valid()) {
        cam.frame(bounds, glm::radians(45.0f));
    }
}

auto ViewFrame::frameArrived() -> void {
    if (!frameTarget.empty()) {
        auto bb = scene.bounds(frameTarget);
        if (bb.valid()) {
            cam.frame(bb, glm::radians(45.0f));
            return;
        }
    }
    frameScene();
}

auto ViewFrame::placeStartCamera() -> void {
    if (startCamera.empty()) {
        return;
    }
    if (!autoFrame) {
        TRACE_EVENT("Scene", "StartCameraDropped", startCamera).text(std::format("start camera {} dropped: the camera was moved first", startCamera));
        startCamera.clear();
        return;
    }
    for (const auto& camera : scene.cameras()) {
        if (camera.path == startCamera) {
            cam.place(camera.worldTransform);
            autoFrame = false;
            TRACE_EVENT("Scene", "StartCameraPlaced", startCamera)
                .text(std::format("start camera {} placed at frame {}, {:.0f} ms after start", startCamera, frameCounter, trace::msSinceStart()))
                .field("frame", (int64_t) frameCounter)
                .field("since_start_ms", trace::msSinceStart());
            startCamera.clear();
            return;
        }
    }
}

auto ViewFrame::moveCamera(const CameraMove& move, bool orbitHeld) -> void {
    static const uint32_t cameraZoneId = profile::registerSite("Camera", profile::Group::Frame);
    profile::beginZone(cameraZoneId);
    if (cameraModes.orbiting && !orbitHeld) {
        cameraModes.orbiting = false;
    }
    // Following the ground: the controlled camera moves across it, and its eye is set the height above the highest drawn surface under it.
    // The ray starts the height above the eye, so ground that rose by up to the height since the last frame is found.
    auto metersPerUnit = scene.lodMetersPerUnit();
    cam.followHeight.reset();
    cameras.view->followHeight.reset();
    auto& controlled = *cameras.controlled;
    if (viewconfig::follow.get()) {
        controlled.followHeight = (float) (viewconfig::followHeight.get() / metersPerUnit);
    }
    auto camBefore = controlled.position;
    controlled.update(move, dt);
    // WASD carry the orbit's pivot along.
    if (cameraModes.orbiting) {
        cameraModes.pivot += controlled.position - camBefore;
    }
    bool flown = flight.flying() && &controlled == &cam;
    if (viewconfig::follow.get() && controlled.followHeight) {
        (void) viewconfig::followHeight.set(*controlled.followHeight * metersPerUnit);
    }
    if (!viewconfig::follow.get()) {
        cameraModes.ground.reset();
        cameraModes.groundPrim.clear();
    } else if (!cameraModes.orbiting && !flown) {
        PROFILE_ZONE("GroundTest", Frame);
        auto start = std::chrono::steady_clock::now();
        auto up = controlled.worldUp;
        auto height = *controlled.followHeight;
        GroundTestCounts counts;
        sceneBounds.update(scene.renderWorld().meshInstances);
        auto hit = groundBelow(scene.renderWorld(), sceneBounds, groundMeshes, controlled.position + (up * height), up, counts);
        if (hit) {
            controlled.position += up * (glm::dot(hit->point, up) + height - glm::dot(controlled.position, up));
            cameraModes.ground = hit->point;
            cameraModes.groundPrim = scene.primPath(hit->prim);
        } else {
            cameraModes.ground.reset();
            cameraModes.groundPrim.clear();
        }
        cameraModes.groundChunks = counts.chunks;
        cameraModes.groundDraws = counts.draws;
        cameraModes.groundTriangles = counts.triangles;
        cameraModes.groundMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    if (controlled.position != camBefore) {
        autoFrame = false;
    }
    profile::endZone();
}

auto ViewFrame::cull() -> void {
    // Culling runs on the GPU: the snapshot carries the frustum to cull against (live, or frozen so the culled set can be inspected from
    // elsewhere), and the results come back a few frames later.
    PROFILE_ZONE("Cull", Frame);
    cullState.enabled = renderconfig::cullingEnabled.get();
    cullState.frozen = renderconfig::cullingFrozen.get();
    cullViewProj = cullState.update(proj * cameras.cull->viewMatrix());
    if (auto result = renderThread.latestCullResult(); result.has_value()) {
        latestCull = std::move(*result);
    }
}

auto ViewFrame::countLodPops() -> void {
    PROFILE_ZONE_DETAIL("CountLodPops", Frame);
    if (latestCull.lodFrame != lodPopsFrame) {
        lodPopsFrame = latestCull.lodFrame;
        lodPops += latestCull.lodPopped;
    }
}

auto ViewFrame::fitCascades() -> void {
    PROFILE_ZONE("FitCascades", Frame);
    AABB bounds;
    {
        PROFILE_ZONE_DETAIL("UpdateSceneBounds", Frame);
        bounds = sceneBounds.update(scene.renderWorld().meshInstances);
    }
    auto lightDir = pickShadowLightDirection(scene.renderWorld().lights, cam.worldUp);
    PROFILE_ZONE_DETAIL("FitShadowCascades", Frame);
    cascadeCount = fitShadowCascades(renderconfig::shadowSettings(), cameras.shadows->viewMatrix(), proj, projectionNear, lightDir, cam.worldUp, bounds, cascades);
    frozenCorners.reset();
    if (cullState.frozenActive) {
        // The frozen frustum has no far plane: drawn out to the shadow distance.
        frozenCorners = cullState.frozenCorners(projectionDepthAt((float) renderconfig::shadowDistance.get()));
    }
}

auto ViewFrame::drawCameraModes() -> void {
    PROFILE_ZONE_DETAIL("DrawCameraModes", Frame);
    ::drawCameraModes(cameraModes, cameras.controlled->position, scene.lodMetersPerUnit(), debugDraw);
}

auto ViewFrame::readBack() -> void {
    PROFILE_ZONE_DETAIL("ReadBack", Frame);
    picking.appendWatches(readbacks.watches);
    // Capture watches go with the snapshot when they change.
    if (readbacks.watches != sentWatches) {
        sentWatches = readbacks.watches;
        watchesChanged = true;
    }
    readbacks.captures = renderThread.takeCaptureResults();
    std::erase_if(readbacks.captures, [this](const CaptureResult& result) {
        return picking.onCaptureResult(result);
    });
}

auto ViewFrame::buildSnapshot() -> RenderSnapshot {
    static const uint32_t snapshotFillZoneId = profile::registerSite("SnapshotFill", profile::Group::Frame);
    profile::beginZone(snapshotFillZoneId);
    const auto& shadowCamera = *cameras.shadows;
    RenderSnapshot snapshot = {
        .viewMatrix = cameras.view->viewMatrix(),
        .projMatrix = proj,
        .lodEye = glm::vec3(glm::inverse(cameras.lod->viewMatrix())[3]),
        .worldUp = cam.worldUp,
        .windowWidth = width,
        .windowHeight = height,
        .gbufferViewMode = static_cast<GBufferView>(renderconfig::bufferView.get()),
        .hzbLevel = (int) renderconfig::hzbLevel.get(),
        .debugView = static_cast<DebugView>(renderconfig::debugView.get()),
        .showBufferOverlay = renderconfig::bufferOverlay.get(),
        .showShadowOverlay = renderconfig::shadowOverlay.get(),
        .antiAliasing = renderconfig::antiAliasing.get(),
        .depthPrepass = renderconfig::depthPrepass.get(),
        .occlusion = renderconfig::cullingOcclusion.get(),
        .cullCameraSeparate = cameras.cull != cameras.view || cullState.frozenActive,
        .sampler = renderconfig::samplerSettings(),
        .cullEnabled = cullState.enabled,
        .cullViewProj = cullViewProj,
        .readLodStates = readLodStates,
        .lodHysteresis = scene.lodTuning().hysteresis,
        .lodFlashFrames = (uint32_t) std::clamp(std::lround(0.5f / std::max(dt, 1e-4f)), 1L, 127L),
        .shadowSettings = renderconfig::shadowSettings(),
        .ambient = renderconfig::ambient(),
        .sky = renderconfig::skyOn.get(),
        .cascadeCount = cascadeCount,
        .cascades = cascades,
        .cascadeCamera = &shadowCamera != cameras.view ? std::optional<std::pair<glm::vec3, glm::vec3>>({shadowCamera.position, shadowCamera.forward()}) : std::nullopt,
    };
    profile::endZone();
    {
        PROFILE_ZONE("TakeDebugLines", Frame);
        snapshot.debugData = debugDraw.take();
    }
    if (std::exchange(watchesChanged, false)) {
        snapshot.captureWatches = sentWatches;
    }
    return snapshot;
}

auto ViewFrame::plotFrame() -> void {
    PROFILE_ZONE_DETAIL("PlotFrame", Frame);
    // The frame times and GPU usage (the portable GPU busy figure is plotted by the profiler as GPU frames arrive).
    if (auto frame = profile::lastFrame()) {
        PROFILE_PLOT("CPU frame ms", Frame, Number, (double) (frame->endNs - frame->startNs) * 1e-6);
    }
    if (auto driverBusy = profile::gpuDriverBusyPercent()) {
        PROFILE_PLOT("GPU busy (driver)", Frame, Percent, *driverBusy);
    }
    PROFILE_PLOT("Resident memory", Frame, Bytes, profile::residentBytes());

    // The renderer: the GPU scene, the uploads, the frame's commands, the memory heaps.
    auto gpu = renderThread.latestSceneGpuStats();
    PROFILE_PLOT("Draw slots", Render, Number, gpu.drawSlots);
    PROFILE_PLOT("Meshes", Render, Number, gpu.meshes);
    PROFILE_PLOT("Textures", Render, Number, gpu.textures);
    PROFILE_PLOT("Materials", Render, Number, gpu.materials);
    PROFILE_PLOT("Change lists waiting", Render, Number, gpu.changeListsSubmitted - gpu.changeListsDrawn);
    bool uploaded = gpu.uploads.lastFrame != plottedUploadFrame;
    plottedUploadFrame = gpu.uploads.lastFrame;
    PROFILE_PLOT("Upload bytes", Upload, Bytes, uploaded ? gpu.uploads.lastBytes : 0);
    PROFILE_PLOT("Uploads", Upload, Number, uploaded ? gpu.uploads.lastUploads : 0);
    PROFILE_PLOT("Upload queue", Upload, Bytes, gpu.uploads.queuedBytes);
    PROFILE_PLOT("Geometry pool vertices", Upload, Number, gpu.geometryPool.vertices.used);
    PROFILE_PLOT("Geometry pool meshlets", Upload, Number, gpu.geometryPool.meshlets.used);
    PROFILE_PLOT("Draws", Render, Number, gpu.frameCommands.draws);
    PROFILE_PLOT("Dispatches", Render, Number, gpu.frameCommands.dispatches);
    PROFILE_PLOT("Barriers", Render, Number, gpu.frameCommands.barriers);
    PROFILE_PLOT("Pipeline binds", Render, Number, gpu.frameCommands.pipelineBinds);
    PROFILE_PLOT("Indirect draws", Render, Number, gpu.frameCommands.indirectDraws);
    PROFILE_PLOT("Copies", Render, Number, gpu.frameCommands.copies);
    PROFILE_PLOT("Primitives", Render, Number, gpu.frameCommands.primitives);
    while (heapPlots.size() < gpu.heaps.size()) {
        auto index = heapPlots.size();
        auto kind = gpu.heaps[index].deviceLocal ? "device" : "host";
        heapPlots.push_back({
            .usage = profile::registerPlot(std::format("GPU heap {} ({}) usage", index, kind).c_str(), profile::Group::Render, profile::PlotFormat::Bytes),
            .budget = profile::registerPlot(std::format("GPU heap {} ({}) budget", index, kind).c_str(), profile::Group::Render, profile::PlotFormat::Bytes),
            .allocated = profile::registerPlot(std::format("GPU heap {} ({}) allocated", index, kind).c_str(), profile::Group::Render, profile::PlotFormat::Bytes),
        });
    }
    for (size_t i = 0; i < gpu.heaps.size(); i++) {
        profile::plot(heapPlots[i].usage, (double) gpu.heaps[i].usage);
        profile::plot(heapPlots[i].budget, (double) gpu.heaps[i].budget);
        profile::plot(heapPlots[i].allocated, (double) gpu.heaps[i].allocated);
    }

    // Culling, when a readback arrived: what the camera and each cascade drew and culled.
    if (latestCull.frame != 0 && latestCull.frame != plottedCullFrame) {
        plottedCullFrame = latestCull.frame;
        PROFILE_PLOT("Camera instances drawn", Culling, Number, latestCull.cameraDrawn);
        PROFILE_PLOT("Camera instances culled", Culling, Number, latestCull.cameraCulled);
        PROFILE_PLOT("Camera instances occluded", Culling, Number, latestCull.occluded);
        PROFILE_PLOT("Camera meshlets tested", Culling, Number, latestCull.cameraMeshlets.tested);
        PROFILE_PLOT("Camera meshlets frustum culled", Culling, Number, latestCull.cameraMeshlets.frustumCulled);
        PROFILE_PLOT("Camera meshlets cone culled", Culling, Number, latestCull.cameraMeshlets.coneCulled);
        PROFILE_PLOT("Camera meshlets occluded", Culling, Number, latestCull.cameraMeshlets.occluded);
        PROFILE_PLOT("Camera triangles drawn", Culling, Number, latestCull.cameraTriangles);
        constexpr std::array<const char*, maxShadowCascades> drawnNames = {"Cascade 0 instances drawn", "Cascade 1 instances drawn", "Cascade 2 instances drawn", "Cascade 3 instances drawn"};
        constexpr std::array<const char*, maxShadowCascades> trianglesNames = {"Cascade 0 triangles drawn", "Cascade 1 triangles drawn", "Cascade 2 triangles drawn", "Cascade 3 triangles drawn"};
        for (uint32_t c = 0; c < latestCull.cascadeCount && c < maxShadowCascades; c++) {
            profile::plot(profile::registerPlot(drawnNames[c], profile::Group::Culling, profile::PlotFormat::Number), latestCull.cascadeDrawn[c]);
            profile::plot(profile::registerPlot(trianglesNames[c], profile::Group::Culling, profile::PlotFormat::Number), (double) latestCull.cascadeTriangles[c]);
        }
    }

    // The scene thread, as of its latest snapshot.
    const auto& thread = scene.snapshot().thread;
    PROFILE_PLOT("Scene assets in flight", Scene, Number, thread.inFlightLayers + thread.inFlightMeshes + thread.inFlightMaterials + thread.inFlightTextures + thread.inFlightOther);
    PROFILE_PLOT("Placements drawn", Scene, Number, thread.placementsDrawn);
    PROFILE_PLOT("Contents reached", Scene, Number, thread.contentsReached);
    PROFILE_PLOT("LOD groups", Scene, Number, thread.lodGroups);

    // The asset client: what is asked for and not answered, and the bytes received this frame.
    auto client = assets.stats();
    PROFILE_PLOT("Asset requests in flight", Assets, Number, client.inFlight());
    PROFILE_PLOT("Asset bytes received", Assets, Bytes, client.bytes - std::min(client.bytes, plottedAssetBytes));
    plottedAssetBytes = client.bytes;
}
