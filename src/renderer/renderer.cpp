#include "renderer.h"
#include "blitpass.h"
#include "imguibackend.h"
#include "material.h"
#include "mesh.h"
#include "observationmacros.h"
#include "presentpass.h"
#include "profile.h"
#include "rendersnapshot.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "rhiswapchain.h"
#include "shadowpass.h"

#include <array>
#include <cstring>
#include <limits>
#include <print>
#include <span>

auto Renderer::init(RhiDevice* rhiDevice, ImGuiBackend* imguiBackend, RhiExtent2D windowExtent) -> std::expected<void, int> {
    using enum RhiFormat;

    device = rhiDevice;

    swapchain = device->createSwapchain(windowExtent);
    if (swapchain == nullptr) {
        return std::unexpected(1);
    }

    resourcePool.init(device);
    frameGraph.setResourcePool(&resourcePool);
    uploader.init(device);
    deletionQueue.init(device);

    auto imgCount = swapchain->imageCount();
    auto ext = swapchain->extent();
    auto colorFmt = swapchain->colorFormat();
    auto depthFmt = depthFormat;

    recreateDepthTexture(ext);

    // Shared uniform buffers (view/proj)
    uniformBuffers.resize(imgCount);
    uniformBuffersMapped.resize(imgCount);
    for (uint32_t i = 0; i < imgCount; i++) {
        RhiBufferDesc uboDesc = {
            .size = sizeof(UniformBufferObject),
            .usage = RhiBufferUsage::Uniform,
            .memory = RhiMemoryUsage::CpuToGpu,
        };
        uniformBuffers[i] = device->createBuffer(uboDesc);
        uniformBuffersMapped[i] = device->mapBuffer(uniformBuffers[i]);
    }

    // Shared sampler and fallback texture
    textureSampler = device->createSampler({});

    std::vector<uint8_t> fallbackPixels(static_cast<size_t>(64) * 64 * 4);
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            bool pink = (((x / 8) + (y / 8)) % 2) != 0;
            auto i = (y * 64 + x) * 4;
            fallbackPixels[i + 0] = pink ? 255 : 64;
            fallbackPixels[i + 1] = pink ? 0 : 64;
            fallbackPixels[i + 2] = pink ? 128 : 64;
            fallbackPixels[i + 3] = 255;
        }
    }
    RhiTextureDesc fallbackDesc = {
        .width = 64,
        .height = 64,
        .format = R8G8B8A8_SRGB,
    };
    uploader.begin();
    fallbackTexture = uploader.uploadTexture(fallbackDesc, std::as_bytes(std::span(fallbackPixels)));
    uploader.end();

    // Passes
    RhiExtent2D shadowExtent{2048, 2048};
    if (!shadowPass.init(device, shadowExtent, depthFmt)) {
        return std::unexpected(1);
    }
    if (!geometryPass.init(device, ext, depthFmt)) {
        return std::unexpected(1);
    }
    if (!lightingPass.init(device, imgCount, ext, colorFmt)) {
        return std::unexpected(1);
    }
    if (!aaPass.init(device, imgCount, colorFmt)) {
        return std::unexpected(1);
    }
    // Overlays draw on the backbuffer after the AA result has been blitted there,
    // so they keep the swapchain format and stay outside the AA filter.
    if (!debugRenderer.init(device, imgCount, ext, colorFmt, depthFmt, uniformBuffers)) {
        return std::unexpected(1);
    }
    if (!gizmoPass.init(device, imgCount, ext, colorFmt)) {
        return std::unexpected(1);
    }

    // Frame sync
    cmdBuffers.resize(imgCount);
    for (uint32_t i = 0; i < imgCount; i++) {
        cmdBuffers[i] = device->createCommandBuffer();
    }

    imageAvailableSemaphores.resize(imgCount);
    renderFinishedSemaphores.resize(imgCount);
    inflightFences.resize(imgCount);
    slotFrame.assign(imgCount, 0);
    slotSubmitNs.assign(imgCount, 0);
    for (uint32_t i = 0; i < imgCount; i++) {
        imageAvailableSemaphores[i] = device->createSemaphore();
        renderFinishedSemaphores[i] = device->createSemaphore();
        inflightFences[i] = device->createFence(true);
    }

    if (!device->limits().timestamps) {
        std::println("GPU timestamps not supported on this device; pass timings disabled");
    } else {
        std::println("GPU clock calibration: {}", device->limits().calibratedTimestamps ? "available" : "unavailable, GPU lane anchored at submit");
    }

    // Editor UI
    editorUI = imguiBackend;
    editorUI->init({
        .device = device,
        .colorFormat = swapchain->colorFormat(),
        .imageCount = swapchain->imageCount(),
    });

    fgPreviews.init(device, editorUI, textureSampler, &deletionQueue);

    return {};
}

auto Renderer::recreateDepthTexture(RhiExtent2D extent) -> void {
    if (depthTexture != nullptr) {
        device->destroyTexture(depthTexture);
    }
    RhiTextureDesc desc = {
        .width = extent.width,
        .height = extent.height,
        .format = depthFormat,
        .usage = RhiTextureUsage::DepthAttachment | RhiTextureUsage::Sampled,
    };
    depthTexture = device->createTexture(desc);
}

auto Renderer::setFrameGraphDebugEnabled(bool enabled) -> void {
    if (enabled == fgDebugEnabled) {
        return;
    }
    fgDebugEnabled = enabled;
    if (enabled) {
        frameGraph.setDebugCaptureHook([this](RhiCommandBuffer* cmd, const FgCapturedResource& view) { fgPreviews.capture(cmd, view); });
    } else {
        frameGraph.setDebugCaptureHook(nullptr);
    }
}

auto Renderer::buildFrameGraphDebugSnapshot() const -> FrameGraphDebugSnapshot {
    auto snap = frameGraph.buildDebugSnapshot();
    fgPreviews.annotate(snap);
    snap.gpuFrameMs = lastGpuFrameMs;
    for (auto& pass : snap.passes) {
        for (const auto& timing : lastGpuTimes) {
            if (pass.name == timing.name) {
                pass.gpuTimeMs = timing.ms;
                break;
            }
        }
    }
    return snap;
}

// Called once the slot's fence has signalled: the zones recorded on that slot's command
// buffer are complete. Top-level zones are the frame graph passes; nested ones belong to
// whatever a pass timed inside itself.
auto Renderer::readGpuTimings(uint32_t slot) -> void {
    if (!device->limits().timestamps || slotFrame[slot] == 0) {
        return;
    }
    if (!device->collectGpuZones(cmdBuffers[slot], gpuZoneScratch) || gpuZoneScratch.empty()) {
        return;
    }
    lastGpuTimes.clear();
    uint64_t minStart = UINT64_MAX;
    uint64_t maxEnd = 0;
    for (const auto& zone : gpuZoneScratch) {
        minStart = std::min(minStart, zone.startNs);
        maxEnd = std::max(maxEnd, zone.endNs);
        if (zone.depth == 0) {
            lastGpuTimes.push_back({.name = zone.name, .ms = (double) (zone.endNs - zone.startNs) * 1e-6});
        }
    }
    lastGpuFrameMs = (double) (maxEnd - minStart) * 1e-6;
    lastGpuFrame = slotFrame[slot];

    // Hand the same zones to the profiler's GPU lane, on the CPU clock. Calibrated when the
    // device supports it (true position of GPU work relative to the threads); otherwise
    // anchored so the first zone starts at the submit, a lower bound.
    if (device->limits().calibratedTimestamps && (!gpuClockCalibrated || framesSinceCalibration >= 120)) {
        gpuClockCalibrated = device->calibrateGpuClock(gpuClockAtCalibration, cpuClockAtCalibration);
        framesSinceCalibration = 0;
    }
    framesSinceCalibration++;
    auto toCpu = [&](uint64_t gpuNs) -> uint64_t {
        if (gpuClockCalibrated) {
            return gpuNs - gpuClockAtCalibration + cpuClockAtCalibration;
        }
        return gpuNs - minStart + slotSubmitNs[slot];
    };
    std::vector<profile::Zone> profileZones;
    profileZones.reserve(gpuZoneScratch.size());
    for (const auto& zone : gpuZoneScratch) {
        profileZones.push_back({.nameId = profile::registerName(zone.name), .depth = zone.depth, .startNs = toCpu(zone.startNs), .endNs = toCpu(zone.endNs)});
    }
    profile::submitGpuZones(lastGpuFrame, slotSubmitNs[slot], profileZones);

    // Builder emits on destruction, so scope it; one field per pass keeps headless runs self-describing.
    // gpu_lag_ms: submit to first GPU zone start on the CPU clock; only meaningful when calibrated.
    if (obs::bus().categoryEnabled("Render")) {
        obs::detail::Builder event("Render", "GpuTime", "frame");
        event.field("frame", (int64_t) lastGpuFrame).field("gpu_ms", lastGpuFrameMs);
        event.field("calibrated", gpuClockCalibrated);
        event.field("gpu_lag_ms", ((double) toCpu(minStart) - (double) slotSubmitNs[slot]) * 1e-6);
        for (const auto& timing : lastGpuTimes) {
            event.field(timing.name, timing.ms);
        }
    }
}

auto Renderer::uploadRenderWorld(const RenderWorld& world, const MeshLibrary& meshLib, const MaterialLibrary& matLib) -> void {
    using enum RhiDescriptorType;

    lights = world.lights;

    bool instancesChanged = (gpuInstances.size() != world.meshInstances.size());
    if (!instancesChanged) {
        for (size_t m = 0; m < world.meshInstances.size(); m++) {
            const auto& inst = world.meshInstances[m];
            if (gpuInstances[m].mesh != inst.mesh || gpuInstances[m].material != inst.material || gpuInstances[m].transform != inst.worldTransform) {
                instancesChanged = true;
                break;
            }
        }
    }

    if (!instancesChanged) {
        return;
    }

    bool geometryChanged = (gpuInstances.size() != world.meshInstances.size());
    if (!geometryChanged) {
        for (size_t m = 0; m < world.meshInstances.size(); m++) {
            const auto& inst = world.meshInstances[m];
            if (gpuInstances[m].mesh != inst.mesh || gpuInstances[m].material != inst.material || gpuInstances[m].indexOffset != inst.indexOffset || gpuInstances[m].indexCount != inst.indexCount) {
                geometryChanged = true;
                break;
            }
        }
    }

    gpuInstances.resize(world.meshInstances.size());
    for (size_t m = 0; m < world.meshInstances.size(); m++) {
        const auto& inst = world.meshInstances[m];
        gpuInstances[m] = {
            .mesh = inst.mesh,
            .material = inst.material,
            .transform = inst.worldTransform,
            .indexOffset = inst.indexOffset,
            .indexCount = inst.indexCount,
            .primFirst = inst.primFirst,
        };
    }

    if (!geometryChanged) {
        return;
    }

    // Old geometry may still be referenced by frames in flight; destroy it once
    // the frame that last used it has completed.
    for (auto& [idx, cached] : meshCache) {
        deletionQueue.deferBuffer(m_frameIndex, cached.vertexBuffer);
        deletionQueue.deferBuffer(m_frameIndex, cached.indexBuffer);
    }
    meshCache.clear();
    for (auto& [idx, cached] : textureCache) {
        deletionQueue.deferTexture(m_frameIndex, cached.texture);
    }
    textureCache.clear();

    uploader.begin();
    for (const auto& inst : world.meshInstances) {
        if (inst.mesh && !meshCache.contains(inst.mesh.index)) {
            const auto* meshData = meshLib.get(inst.mesh);
            if (meshData && !meshData->vertices.empty()) {
                PROFILE_ZONE("UploadMesh");
                PROFILE_ZONE_VALUE(meshData->vertices.size() * sizeof(Vertex) + meshData->indices.size() * sizeof(uint32_t));
                CachedMesh cached;
                cached.indexCount = (uint32_t) meshData->indices.size();

                cached.vertexBuffer = uploader.uploadBuffer(std::as_bytes(std::span(meshData->vertices)), RhiBufferUsage::Vertex);
                cached.indexBuffer = uploader.uploadBuffer(std::as_bytes(std::span(meshData->indices)), RhiBufferUsage::Index);

                meshCache[inst.mesh.index] = cached;

                OBS_EVENT("Render", "MeshUploaded", "Mesh")
                    .field("vertex_count", (int64_t) meshData->vertices.size())
                    .field("index_count", (int64_t) meshData->indices.size());
            }
        }

        if (inst.material && !textureCache.contains(inst.material.index)) {
            const auto* matData = matLib.get(inst.material);
            if (matData && !matData->texPixels.empty()) {
                PROFILE_ZONE("UploadTexture");
                PROFILE_ZONE_VALUE((uint64_t) matData->texWidth * (uint64_t) matData->texHeight * 4);
                RhiTextureDesc texDesc = {
                    .width = (uint32_t) matData->texWidth,
                    .height = (uint32_t) matData->texHeight,
                    .format = RhiFormat::R8G8B8A8_SRGB,
                };
                auto pixelCount = (size_t) matData->texWidth * (size_t) matData->texHeight * 4;
                auto pixels = std::span(matData->texPixels).first(pixelCount);
                textureCache[inst.material.index] = {.texture = uploader.uploadTexture(texDesc, std::as_bytes(pixels))};

                OBS_EVENT("Render", "TextureUploaded", "Texture").field("width", (int64_t) matData->texWidth).field("height", (int64_t) matData->texHeight);
            }
        }
    }

    {
        PROFILE_ZONE("UploadSubmit");
        uploader.end();
    }

    PROFILE_ZONE("DescriptorSets");
    // Sets and pool may still be bound by frames in flight: free and destroy together, later.
    if (geometryDescriptorPool) {
        deletionQueue.defer(m_frameIndex, [dev = device, pool = geometryDescriptorPool, sets = geometryDescriptorSets] {
            dev->freeDescriptorSets(pool, sets);
            dev->destroyDescriptorPool(pool);
        });
        geometryDescriptorPool = nullptr;
    }
    geometryDescriptorSets.clear();

    auto instanceCount = (uint32_t) gpuInstances.size();
    auto imgCount = swapchain->imageCount();
    if (instanceCount == 0) {
        return;
    }

    auto totalSets = imgCount * instanceCount;
    std::array<RhiDescriptorBinding, 2> poolBindings = {{
        {.binding = 0, .type = UniformBuffer, .stage = RhiShaderStage::Vertex},
        {.binding = 1, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},
    }};
    geometryDescriptorPool = device->createDescriptorPool(totalSets, poolBindings);
    geometryDescriptorSets.assign(totalSets, nullptr);
    device->allocateDescriptorSets(geometryDescriptorPool, geometryPass.descriptorSetLayout(), geometryDescriptorSets);

    for (uint32_t i = 0; i < imgCount; i++) {
        for (uint32_t m = 0; m < instanceCount; m++) {
            auto& inst = gpuInstances[m];
            auto* tex = fallbackTexture;
            auto texIt = textureCache.find(inst.material.index);
            if (texIt != textureCache.end()) {
                tex = texIt->second.texture;
            }

            std::array<RhiDescriptorWrite, 2> writes = {{
                {
                    .binding = 0,
                    .type = UniformBuffer,
                    .buffer = uniformBuffers[i],
                    .bufferRange = sizeof(UniformBufferObject),
                },
                {
                    .binding = 1,
                    .type = CombinedImageSampler,
                    .texture = tex,
                    .sampler = textureSampler,
                },
            }};
            device->updateDescriptorSet(geometryDescriptorSets[(i * instanceCount) + m], writes);
        }
    }
}

auto Renderer::initGizmos(Camera* camera) -> void {
    axisGizmo = Axis3DGizmo(camera);
}

auto Renderer::gizmoUpdate(const RenderSnapshot& snapshot, RhiExtent2D extent) -> std::vector<GizmoDrawRequest> {
    std::vector<GizmoDrawRequest> requests;
    if (snapshot.showGizmo) {
        axisGizmo.updateHover(snapshot.mouseX, snapshot.mouseY);
        requests.push_back(axisGizmo.draw(extent, snapshot.viewMatrix));
    }
    if (!snapshot.translateGizmoVerts.empty()) {
        requests.push_back({
            .vertices = snapshot.translateGizmoVerts,
            .viewProj = snapshot.projMatrix * snapshot.viewMatrix,
            .vpExtent = extent,
        });
    }
    if (!snapshot.rotateGizmoVerts.empty()) {
        requests.push_back({
            .vertices = snapshot.rotateGizmoVerts,
            .viewProj = snapshot.projMatrix * snapshot.viewMatrix,
            .vpExtent = extent,
        });
    }
    if (!snapshot.scaleGizmoVerts.empty()) {
        requests.push_back({
            .vertices = snapshot.scaleGizmoVerts,
            .viewProj = snapshot.projMatrix * snapshot.viewMatrix,
            .vpExtent = extent,
        });
    }
    return requests;
}

auto Renderer::gizmoHitTest(float mouseX, float mouseY, RhiExtent2D windowExtent) -> bool {
    return axisGizmo.hitTest(mouseX, mouseY, windowExtent);
}

auto Renderer::render(RenderSnapshot& snapshot) -> void {
    auto frame = ++m_frameIndex;
    OBS_EVENT("Render", "FrameBegin", "frame").field("frame", (int64_t) frame);

    {
        PROFILE_ZONE("WaitFence");
        device->waitForFence(inflightFences[currentFrame]);
    }
    // This slot's previous frame is done, and so is everything submitted before it.
    deletionQueue.flush(slotFrame[currentFrame]);
    readGpuTimings(currentFrame);
    fgPreviews.setFrame(frame);

    std::expected<uint32_t, RhiError> index;
    {
        PROFILE_ZONE("Acquire");
        index = swapchain->acquireNextImage(imageAvailableSemaphores[currentFrame]);
    }
    if (!index) {
        if (index.error() == RhiError::OutOfDate) {
            OBS_EVENT("Render", "SwapchainRecreate", "swapchain").field("reason", "acquire_failed");
            if (!swapchain->recreate({.width = (uint32_t) snapshot.windowWidth, .height = (uint32_t) snapshot.windowHeight})) {
                std::println(stderr, "Swapchain recreate failed after acquire");
            } else {
                recreateDepthTexture(swapchain->extent());
            }
            resourcePool.flush();
            currentFrame = 0;
        }
        OBS_EVENT("Render", "FrameEnd", "frame").field("frame", (int64_t) frame);
        return;
    }

    device->resetFence(inflightFences[currentFrame]);

    UniformBufferObject ubo = {
        .view = snapshot.viewMatrix,
        .proj = snapshot.projMatrix,
    };
    memcpy(uniformBuffersMapped[currentFrame], &ubo, sizeof(ubo));

    // Build frame graph
    static const uint32_t buildZoneId = profile::registerName("BuildFrameGraph");
    profile::beginZone(buildZoneId);
    auto ext = swapchain->extent();
    frameGraph.reset();

    auto colorHandle = frameGraph.importTexture("backbuffer", swapchain->image(*index), {ext.width, ext.height, swapchain->colorFormat()});
    auto depthHandle = frameGraph.importTexture("depth", depthTexture, {ext.width, ext.height, depthFormat, RhiTextureUsage::DepthAttachment | RhiTextureUsage::Sampled});

    // Per-frame resources (UBOs, descriptor sets, dynamic vertex buffers) are owned by
    // the frame slot whose fence guards them, not by the swapchain image.
    auto imageIdx = currentFrame;
    auto instanceCount = (uint32_t) gpuInstances.size();

    static const uint32_t shadowSetupZoneId = profile::registerName("ShadowSetup");
    profile::beginZone(shadowSetupZoneId);
    RhiExtent2D shadowExtent{2048, 2048};

    // Pick a shadow-casting directional light: first directional with shadowEnable,
    // else first directional. USDScene authors a session-layer UsdLuxDistantLight when
    // the scene has none, so this search almost always succeeds.
    const RenderLight* shadowLight = nullptr;
    const RenderLight* fallbackDirectional = nullptr;
    for (const auto& l : lights) {
        if (l.type != LightType::Directional) {
            continue;
        }
        if (fallbackDirectional == nullptr) {
            fallbackDirectional = &l;
        }
        if (l.shadowEnable && shadowLight == nullptr) {
            shadowLight = &l;
        }
    }
    const RenderLight* picked = shadowLight != nullptr ? shadowLight : fallbackDirectional;

    // Resolve into the small value struct LightingPass consumes. If no directional light
    // exists at all (pathological — USDScene would usually author one) just aim the sun
    // straight up the scene's up axis with unit radiance.
    LightingInputs lighting;
    if (picked != nullptr) {
        auto raw = glm::vec3(picked->worldTransform[2]);
        lighting.direction = glm::dot(raw, raw) > 1e-6f ? glm::normalize(raw) : snapshot.worldUp;
        lighting.radiance = picked->color * (picked->intensity * std::exp2(picked->exposure));
        lighting.shadowColor = picked->shadowColor;
    } else {
        lighting.direction = snapshot.worldUp;
    }

    // Fit a scene-bounding sphere around the instance origins, then size the ortho frustum to
    // that sphere with some padding. Instance origins are a coarse approximation of scene
    // bounds (ignores per-mesh extent) but good enough for a first-pass shadow frustum.
    glm::vec3 sceneMin{std::numeric_limits<float>::max()};
    glm::vec3 sceneMax{std::numeric_limits<float>::lowest()};
    for (const auto& inst : gpuInstances) {
        glm::vec3 p{inst.transform[3]};
        sceneMin = glm::min(sceneMin, p);
        sceneMax = glm::max(sceneMax, p);
    }
    glm::vec3 sceneCenter{0.0f};
    float sceneRadius = 1.0f;
    if (!gpuInstances.empty()) {
        sceneCenter = (sceneMin + sceneMax) * 0.5f;
        sceneRadius = glm::length(sceneMax - sceneMin) * 0.5f + 1.0f; // +1 as safety padding
    }
    // Pad generously so per-mesh extents that stick out of instance-origin bounds still fit.
    float shadowHalf = sceneRadius * 2.0f;
    float lightDistance = sceneRadius * 4.0f + 1.0f;

    auto lightPos = sceneCenter + lighting.direction * lightDistance;
    // glm::lookAt is degenerate when the light direction is parallel to the up vector — the
    // cross product to compute "right" becomes zero. That's common for a sun shining straight
    // down; fall back to a perpendicular axis in that case.
    auto shadowUp =
        std::abs(glm::dot(lighting.direction, snapshot.worldUp)) > 0.99f
            ? glm::normalize(glm::cross(lighting.direction, glm::vec3(1.0f, 0.0f, 0.0f)))
            : snapshot.worldUp;
    auto lightView = glm::lookAt(lightPos, sceneCenter, shadowUp);
    auto lightProj = glm::ortho(-shadowHalf, shadowHalf, -shadowHalf, shadowHalf, 0.1f, 2.0f * lightDistance);
    auto lightViewProj = lightProj * lightView;

    auto invViewProj = glm::inverse(snapshot.projMatrix * snapshot.viewMatrix);

    profile::endZone(); // ShadowSetup
    const auto& shadowData = shadowPass.addPass(frameGraph, shadowExtent, depthFormat, lightViewProj, gpuInstances, meshCache);

    const auto& geomData = geometryPass.addPass(frameGraph, depthHandle, ext, imageIdx, instanceCount, gpuInstances, meshCache, geometryDescriptorSets);

    const auto& lightData = lightingPass.addPass(
        frameGraph,
        geomData,
        depthHandle,
        shadowData.shadowMap,
        ext,
        imageIdx,
        textureSampler,
        lighting,
        snapshot.gbufferViewMode,
        snapshot.showBufferOverlay,
        snapshot.showShadowOverlay,
        invViewProj,
        lightViewProj);

    // AA filters only the lit scene. Its output is blitted to the backbuffer and the
    // overlays (debug lines, gizmos, UI) draw on the backbuffer afterwards, so they are
    // never filtered and sceneColorAA holds nothing but the AA result.
    if (snapshot.antiAliasing != lastAntiAliasing) {
        lastAntiAliasing = snapshot.antiAliasing;
        OBS_EVENT("Render", "AntiAliasing", "aa").field("enabled", snapshot.antiAliasing ? "true" : "false");
    }
    const auto& aaData = aaPass.addPass(frameGraph, lightData.sceneColor, ext, imageIdx, textureSampler, snapshot.antiAliasing);

    addBlitPass(frameGraph, "BlitToBackbuffer", aaData.sceneColorAA, colorHandle, ext, ext);

    debugRenderer.addPass(frameGraph, colorHandle, depthHandle, ext, snapshot.debugData, imageIdx);

    static const uint32_t gizmoZoneId = profile::registerName("GizmoUpdate");
    profile::beginZone(gizmoZoneId);
    auto gizmoRequests = gizmoUpdate(snapshot, ext);
    profile::endZone(); // GizmoUpdate
    gizmoPass.addPass(frameGraph, colorHandle, ext, gizmoRequests, imageIdx);

    editorUIPass.addPass(frameGraph, colorHandle, ext, editorUI, snapshot.imguiSnapshot);

    addPresentPass(frameGraph, colorHandle);

    profile::zoneValue(instanceCount);
    profile::endZone(); // BuildFrameGraph
    {
        PROFILE_ZONE("Compile");
        frameGraph.compile();
    }
    OBS_EVENT("Render", "FrameGraphCompiled", "frame")
        .field("pass_count", (int64_t) frameGraph.passCount())
        .field("culled_count", (int64_t) frameGraph.culledCount());

    // Command buffer and fence belong to the frame slot; the render-finished semaphore
    // belongs to the swapchain image, since present consumes it per image.
    auto* cmd = cmdBuffers[currentFrame];
    {
        PROFILE_ZONE("Record");
        PROFILE_ZONE_VALUE(frameGraph.passCount() - frameGraph.culledCount());
        cmd->reset();
        cmd->begin();
        frameGraph.execute(cmd);
        cmd->end();
    }

    RhiSubmitInfo submitInfo = {
        .waitSemaphore = imageAvailableSemaphores[currentFrame],
        .signalSemaphore = renderFinishedSemaphores[*index],
        .fence = inflightFences[currentFrame],
    };
    {
        PROFILE_ZONE("Submit");
        device->submitCommandBuffer(cmd, submitInfo);
    }
    slotFrame[currentFrame] = frame;
    slotSubmitNs[currentFrame] = profile::now();
    std::expected<void, RhiError> presented;
    {
        PROFILE_ZONE("Present");
        presented = device->present(swapchain, renderFinishedSemaphores[*index], *index);
    }
    if (!presented) {
        OBS_EVENT("Render", "SwapchainRecreate", "swapchain").field("reason", "present_failed");
        if (!swapchain->recreate({.width = (uint32_t) snapshot.windowWidth, .height = (uint32_t) snapshot.windowHeight})) {
            std::println(stderr, "Swapchain recreate failed after present");
        } else {
            recreateDepthTexture(swapchain->extent());
        }
        resourcePool.flush();
        currentFrame = 0;
        OBS_EVENT("Render", "FrameEnd", "frame").field("frame", (int64_t) frame);
        return;
    }

    currentFrame = (currentFrame + 1) % swapchain->imageCount();
    OBS_EVENT("Render", "FrameEnd", "frame").field("frame", (int64_t) frame);
}

auto Renderer::destroy() -> void {
    device->waitIdle();
    deletionQueue.flushAll();
    uploader.destroy();

    fgPreviews.shutdown();

    editorUI->shutdown();
    editorUI = nullptr;

    if (geometryDescriptorPool) {
        device->freeDescriptorSets(geometryDescriptorPool, geometryDescriptorSets);
        device->destroyDescriptorPool(geometryDescriptorPool);
    }

    for (auto& [idx, cached] : meshCache) {
        device->destroyBuffer(cached.vertexBuffer);
        device->destroyBuffer(cached.indexBuffer);
    }
    for (auto& [idx, cached] : textureCache) {
        device->destroyTexture(cached.texture);
    }
    meshCache.clear();
    textureCache.clear();

    shadowPass.destroy(device);
    geometryPass.destroy(device);
    lightingPass.destroy(device);
    aaPass.destroy(device);
    debugRenderer.destroy(device);
    gizmoPass.destroy(device);

    for (uint32_t i = 0; i < swapchain->imageCount(); i++) {
        device->unmapBuffer(uniformBuffers[i]);
        device->destroyBuffer(uniformBuffers[i]);
    }

    device->destroySampler(textureSampler);
    device->destroyTexture(fallbackTexture);
    device->destroyTexture(depthTexture);
    depthTexture = nullptr;

    for (uint32_t i = 0; i < swapchain->imageCount(); i++) {
        device->destroySemaphore(imageAvailableSemaphores[i]);
        device->destroySemaphore(renderFinishedSemaphores[i]);
        device->destroyFence(inflightFences[i]);
        device->destroyCommandBuffer(cmdBuffers[i]);
    }

    resourcePool.destroy();

    swapchain->destroy();
    delete swapchain;
}
