#include "renderer.h"

#include "renderconfig.h"

#include "blitpass.h"
#include "instanceuploadpass.h"
#include "material.h"
#include "mesh.h"
#include "mipchain.h"
#include "presentpass.h"
#include "previewtextures.h"
#include "profile.h"
#include "projection.h"
#include "rendersnapshot.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "rhiswapchain.h"
#include "shadowpass.h"
#include "trace.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <limits>
#include <span>

auto Renderer::addExtension(RenderExtension& extension) -> void {
    extension.extensionIndex = (uint32_t) extensions.size();
    extensions.push_back(&extension);
}

auto Renderer::init(RhiDevice* rhiDevice, AssetClient* shaders, RhiExtent2D windowExtent) -> std::expected<void, int> {
    using enum RhiFormat;

    device = rhiDevice;
    pipelineRegistry.init(device, shaders, &deletionQueue);

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
        device->setDebugName(uniformBuffers[i], "renderer.view.ubo");
        uniformBuffersMapped[i] = device->mapBuffer(uniformBuffers[i]);
    }

    // Shared sampler and fallback texture
    textureSampler = device->createSampler({.debugName = "sampler.fullscreen"});
    // Shadow compare sampler: linear so the hardware compare is bilinear PCF, clamped so a
    // tile never reads its neighbour, LessOrEqual on the biased reference depth.
    shadowSampler = device->createSampler({
        .addressU = RhiAddressMode::ClampToEdge,
        .addressV = RhiAddressMode::ClampToEdge,
        .addressW = RhiAddressMode::ClampToEdge,
        .compareEnable = true,
        .compareOp = RhiCompareOp::LessOrEqual,
        .debugName = "sampler.shadow",
    });
    // Material sampler: trilinear with anisotropy, the textures carry full mip chains and
    // Sponza floors are seen at grazing angles. Rebuilt when the settings change.
    materialSampler = device->createSampler(toSamplerDesc(materialSamplerSettings));
    device->setDebugName(materialSampler, "sampler.material");

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
        .debugName = "texture.fallback",
    };
    // White, sampled by a material while its texture is still uploading.
    std::array<uint8_t, 4> pendingPixels = {255, 255, 255, 255};
    RhiTextureDesc pendingDesc = {
        .width = 1,
        .height = 1,
        .format = R8G8B8A8_SRGB,
        .debugName = "texture.pending",
    };
    uploader.begin();
    fallbackTexture = uploader.uploadTexture(fallbackDesc, std::as_bytes(std::span(fallbackPixels)));
    pendingTexture = uploader.uploadTexture(pendingDesc, std::as_bytes(std::span(pendingPixels)));
    uploader.end();

    // Passes
    RhiExtent2D shadowExtent{2048, 2048};
    debugShadowExtent = shadowExtent;
    if (!geometryPass.init(device, pipelineRegistry, ext, depthFmt)) {
        return std::unexpected(1);
    }
    if (!shadowPass.init(pipelineRegistry, depthFmt, geometryPass.descriptorSetLayout())) {
        return std::unexpected(1);
    }
    if (!depthPrepass.init(pipelineRegistry, depthFmt, geometryPass.descriptorSetLayout())) {
        return std::unexpected(1);
    }
    if (!instanceCullPass.init(device, pipelineRegistry)) {
        return std::unexpected(1);
    }
    if (!lodSelectPass.init(device, pipelineRegistry)) {
        return std::unexpected(1);
    }
    if (!depthPyramid.init(device, pipelineRegistry)) {
        return std::unexpected(1);
    }
    depthPyramid.resize(ext, depthTexture, cullDepthTexture);
    if (!lightingPass.init(device, pipelineRegistry, imgCount, ext, colorFmt)) {
        return std::unexpected(1);
    }
    if (!aaPass.init(device, pipelineRegistry, imgCount, colorFmt)) {
        return std::unexpected(1);
    }
    if (!debugViewPass.init(device, pipelineRegistry, imgCount, depthFmt, geometryPass.descriptorSetLayout())) {
        return std::unexpected(1);
    }
    // Overlays draw on the backbuffer after the AA result has been blitted there,
    // so they keep the swapchain format and stay outside the AA filter.
    if (!debugRenderer.init(device, pipelineRegistry, imgCount, ext, colorFmt, depthFmt, uniformBuffers)) {
        return std::unexpected(1);
    }

    // Scene GPU tables; the instance buffer exists from here on, grown by applySceneChanges.
    gpuScene.init(device, imgCount, &deletionQueue, fallbackTexture, pendingTexture);
    drawLists.init(device, imgCount, &deletionQueue);
    lodGroups.init(device, imgCount, &deletionQueue);
    boundInstanceGeneration = gpuScene.instanceGeneration();

    // Frame sync
    cmdBuffers.resize(imgCount);
    for (uint32_t i = 0; i < imgCount; i++) {
        cmdBuffers[i] = device->createCommandBuffer();
        device->setDebugName(cmdBuffers[i], std::format("frameslot{}.cmd", i).c_str());
    }

    imageAvailableSemaphores.resize(imgCount);
    renderFinishedSemaphores.resize(imgCount);
    inflightFences.resize(imgCount);
    slotFrame.assign(imgCount, 0);
    slotSubmitNs.assign(imgCount, 0);
    slotDrawLogs.resize(imgCount);
    for (uint32_t i = 0; i < imgCount; i++) {
        imageAvailableSemaphores[i] = device->createSemaphore();
        renderFinishedSemaphores[i] = device->createSemaphore();
        inflightFences[i] = device->createFence(true);
        device->setDebugName(imageAvailableSemaphores[i], std::format("frameslot{}.imageavailable", i).c_str());
        device->setDebugName(renderFinishedSemaphores[i], std::format("swapchainimage{}.renderfinished", i).c_str());
        device->setDebugName(inflightFences[i], std::format("frameslot{}.inflight", i).c_str());
    }

    if (!device->limits().timestamps) {
        TRACE_WARNING("Render", "TimestampsUnavailable", "device").text("GPU timestamps not supported on this device; pass timings disabled");
    } else {
        TRACE_EVENT("Render", "GpuClockCalibration", "device")
            .text(std::format("GPU clock calibration: {}", device->limits().calibratedTimestamps ? "available" : "unavailable, GPU lane anchored at submit"))
            .field("available", device->limits().calibratedTimestamps);
    }

    captureService.init(device, previewTextures, &deletionQueue, &uploader, imgCount);

    // What joins the frame from outside the core: the instrumentation, the tools' gizmos, the UI.
    RenderExtensionContext context = {
        .device = *device,
        .pipelines = pipelineRegistry,
        .frameGraph = frameGraph,
        .deletionQueue = deletionQueue,
        .uploader = uploader,
        .textureSampler = textureSampler,
        .previews = previewTextures,
        .frameSlots = imgCount,
        .colorFormat = swapchain->colorFormat(),
    };
    for (auto* extension : extensions) {
        if (!extension->init(context)) {
            return std::unexpected(1);
        }
    }
    return {};
}

auto Renderer::nameSwapchainImages() -> void {
    for (uint32_t i = 0; i < swapchain->imageCount(); i++) {
        device->setDebugName(swapchain->image(i), std::format("swapchain.image{}", i).c_str());
    }
}

auto Renderer::recreateDepthTexture(RhiExtent2D extent) -> void {
    nameSwapchainImages();
    if (depthTexture != nullptr) {
        device->destroyTexture(depthTexture);
    }
    RhiTextureDesc desc = {
        .width = extent.width,
        .height = extent.height,
        .format = depthFormat,
        // TransferSrc: the capture service and frame debugger read it back.
        .usage = RhiTextureUsage::DepthAttachment | RhiTextureUsage::Sampled | RhiTextureUsage::TransferSrc,
        .debugName = "renderer.depth",
    };
    depthTexture = device->createTexture(desc);
    // Occlusion culling's depth while the culling camera isn't the rendered one: drawn from the culling camera, read only by the pyramid.
    if (cullDepthTexture != nullptr) {
        device->destroyTexture(cullDepthTexture);
    }
    desc.debugName = "renderer.depth.cull";
    cullDepthTexture = device->createTexture(desc);
    // The pyramid follows the depth buffers' size and reads them; made in init once its pipeline is registered.
    if (depthPyramid.texture() != nullptr) {
        depthPyramid.resize(extent, depthTexture, cullDepthTexture);
        descriptorSetsDirty = true;
    }
}

auto Renderer::requestShaders() -> size_t {
    pipelineRegistry.requestAtStart();
    return pipelineRegistry.atStartShaders().size();
}

static auto toRenderDebug(const GpuScene::GeometryPoolStats& pool) -> RenderDebugGeometryPool {
    auto use = [](const GpuScene::PoolUse& part) {
        return RenderDebugPoolUse{.limit = part.limit, .capacity = part.capacity, .used = part.used};
    };
    return {
        .limitBytes = pool.limitBytes,
        .vertices = use(pool.vertices),
        .meshlets = use(pool.meshlets),
        .meshletVertices = use(pool.meshletVertices),
        .meshletTriangles = use(pool.meshletTriangles),
        .waitingMeshes = pool.waitingMeshes,
        .waitingBytes = pool.waitingBytes,
    };
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

    lastDrawLog = slotDrawLogs[slot];

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
}

// The backbuffer cleared to the lit image's background: the frame while the lit path's pipelines are still on their way.
static auto addClearPass(FrameGraph& fg, FgTextureHandle color, RhiExtent2D extent) -> void {
    struct ClearData {
        FgTextureHandle color;
    };
    fg.addPass<ClearData>(
        "ClearBackbuffer",
        [&](FrameGraphBuilder& builder, ClearData& data) { data.color = builder.write(color, FgAccessFlags::ColorAttachment); },
        [extent](FrameGraphContext& ctx, const ClearData& data) {
            std::array<RhiRenderingAttachmentInfo, 1> attachments = {{
                {
                    .texture = ctx.texture(data.color),
                    .state = RhiTextureState::ColorAttachment,
                    .clear = true,
                    .clearColor = {0.12f, 0.12f, 0.15f, 1.0f},
                },
            }};
            ctx.cmd()->beginRendering({.extent = extent, .colorAttachments = {attachments.data(), attachments.size()}});
            ctx.cmd()->endRendering();
        });
}

// A byte count for the trace: "812 B", "10.2 MiB".
static auto formatUploadBytes(uint64_t bytes) -> std::string {
    if (bytes < 1024 * 1024) {
        return std::format("{:.1f} KiB", (double) bytes / 1024.0);
    }
    return std::format("{:.1f} MiB", (double) bytes / (1024.0 * 1024.0));
}

auto Renderer::applySceneChanges(const SceneChanges& changes) -> void {
    auto start = std::chrono::steady_clock::now();
    // Removals first: their pool ranges, table entries and slots are free for what is added (a repacked scene reuses them instead of
    // growing the pool). Nothing draws between here and the draws below, which stop naming what was removed.
    {
        PROFILE_ZONE("RemoveEntries");
        for (auto handle : changes.materialsRemoved) {
            gpuScene.removeMaterial(handle);
        }
        for (auto handle : changes.meshesRemoved) {
            gpuScene.removeMesh(handle);
        }
        for (auto handle : changes.texturesRemoved) {
            gpuScene.removeTexture(handle, m_frameIndex);
        }
    }
    // Textures and meshes before the materials and draws that name them.
    {
        PROFILE_ZONE("AddTextures");
        for (const auto& [handle, texture] : changes.texturesAdded) {
            gpuScene.addTexture(handle, texture, m_frameIndex);
        }
    }
    {
        PROFILE_ZONE("AddMeshes");
        for (const auto& [handle, mesh] : changes.meshesAdded) {
            gpuScene.addMesh(handle, mesh);
        }
    }
    for (const auto& [handle, material] : changes.materialsChanged) {
        gpuScene.setMaterial(handle, material);
    }
    static const uint32_t copyZoneId = profile::registerName("CopyInstances");
    profile::beginZone(copyZoneId);
    // Slots past the new count leave the bucket counts with them (the slot list doesn't shrink today).
    for (size_t slot = changes.slotCount; slot < gpuInstances.size(); slot++) {
        drawLists.countInstance(gpuInstances[slot], -1);
        sceneTriangles -= gpuInstances[slot].indexCount / 3;
    }
    gpuInstances.resize(changes.slotCount);
    std::vector<uint32_t> changedSlots;
    changedSlots.reserve(changes.instances.size());
    for (const auto& [slot, inst] : changes.instances) {
        if (slot >= gpuInstances.size()) {
            continue;
        }
        drawLists.countInstance(gpuInstances[slot], -1);
        sceneTriangles -= gpuInstances[slot].indexCount / 3;
        gpuInstances[slot] = {
            .mesh = inst.mesh,
            .material = inst.material,
            .prim = inst.prim,
            .transform = inst.worldTransform,
            .indexOffset = inst.indexOffset,
            .indexCount = inst.indexCount,
            .meshletOffset = inst.meshletOffset,
            .meshletCount = inst.meshletCount,
            .doubleSided = inst.doubleSided,
            .cutout = inst.cutout,
            .lodLevel = inst.lodLevel,
            .lodDepth = inst.lodDepth,
            .lodGroup = inst.lodGroup,
            .streamRoot = inst.streamRoot,
            .worldBounds = inst.worldBounds,
            .alive = inst.alive,
        };
        drawLists.countInstance(gpuInstances[slot], 1);
        sceneTriangles += gpuInstances[slot].indexCount / 3;
        changedSlots.push_back(slot);
    }
    drawLists.updateGroupBounds(gpuInstances, changedSlots);
    profile::endZone();
    if (changes.lightsChanged) {
        lights = changes.lights;
    }
    if (changes.environmentChanged) {
        environment = changes.environment;
    }

    static const uint32_t boundsZoneId = profile::registerName("SceneBounds");
    profile::beginZone(boundsZoneId);
    sceneBounds = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
    for (const auto& inst : gpuInstances) {
        if (inst.alive && inst.worldBounds.valid()) {
            sceneBounds.min = glm::min(sceneBounds.min, inst.worldBounds.min);
            sceneBounds.max = glm::max(sceneBounds.max, inst.worldBounds.max);
        }
    }

    profile::endZone();

    // A grown instance buffer or grown draw lists are new buffers: the descriptor sets naming them are rewritten at the next frame.
    {
        PROFILE_ZONE("UpdateInstances");
        gpuScene.updateInstances(gpuInstances, changedSlots, m_frameIndex);
    }
    {
        PROFILE_ZONE("GrowDrawLists");
        if (drawLists.ensureCapacity((uint32_t) gpuInstances.size(), drawLists.commandsNeeded(), m_frameIndex)) {
            descriptorSetsDirty = true;
        }
        // The meshlet work items: room for every live instance's meshlets twice (the camera and the cascades see a part each), grown
        // further if a frame overflows.
        if (drawLists.ensureWorkCapacity(drawLists.liveMeshlets() * 2, m_frameIndex)) {
            descriptorSetsDirty = true;
        }
    }
    {
        PROFILE_ZONE("ApplyLodGroups");
        if (lodGroups.apply(changes.lodGroups, changes.lodGroupCount, m_frameIndex)) {
            descriptorSetsDirty = true;
        }
    }
    if (gpuScene.instanceGeneration() != boundInstanceGeneration) {
        boundInstanceGeneration = gpuScene.instanceGeneration();
        descriptorSetsDirty = true;
    }

    if (changes.meshesAdded.empty() && changes.texturesAdded.empty() && changes.materialsChanged.empty() && changes.meshesRemoved.empty() && changes.texturesRemoved.empty() &&
        changes.materialsRemoved.empty()) {
        return;
    }
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    TRACE_EVENT("Render", "SceneChangesApplied", "RenderWorld")
        .text(std::format("scene changes: {} draws, {} meshes added and {} removed, {} textures added and {} removed, {} materials set and {} removed; {} uploads ({}) queued, in {:.0f} ms",
                          changes.instances.size(),
                          changes.meshesAdded.size(),
                          changes.meshesRemoved.size(),
                          changes.texturesAdded.size(),
                          changes.texturesRemoved.size(),
                          changes.materialsChanged.size(),
                          changes.materialsRemoved.size(),
                          gpuScene.pendingUploads(),
                          formatUploadBytes(gpuScene.pendingBytes()),
                          ms))
        .field("draws", (int64_t) changes.instances.size())
        .field("meshesAdded", (int64_t) changes.meshesAdded.size())
        .field("meshesRemoved", (int64_t) changes.meshesRemoved.size())
        .field("texturesAdded", (int64_t) changes.texturesAdded.size())
        .field("texturesRemoved", (int64_t) changes.texturesRemoved.size())
        .field("materialsSet", (int64_t) changes.materialsChanged.size())
        .field("materialsRemoved", (int64_t) changes.materialsRemoved.size())
        .field("pendingBytes", (int64_t) gpuScene.pendingBytes())
        .field("ms", ms);
}

auto Renderer::sceneGpuStats() const -> SceneGpuStats {
    auto uploads = gpuScene.uploadStats();
    auto pool = gpuScene.geometryPoolStats();
    return {
        .frame = m_frameIndex,
        .drawSlots = (uint32_t) gpuInstances.size(),
        .meshes = (uint32_t) gpuScene.meshRanges().size(),
        .textures = (uint32_t) gpuScene.textures().size(),
        .materials = gpuScene.materialCount(),
        .uploads = {
            .queued = uploads.queued,
            .queuedBytes = uploads.queuedBytes,
            .budget = uploads.budget,
            .lastUploads = uploads.lastUploads,
            .lastBytes = uploads.lastBytes,
            .lastFrame = uploads.lastFrame,
            .totalBytes = uploads.totalBytes,
            .texturesWaitingSlot = uploads.texturesWaitingSlot,
            .instanceRecords = uploads.instanceRecords,
            .instanceRegions = uploads.instanceRegions,
        },
        .geometryPool = toRenderDebug(pool),
        .skyDrawn = debugSky,
    };
}

auto Renderer::flushSceneUploads() -> void {
    auto start = std::chrono::steady_clock::now();
    auto bytes = gpuScene.pendingBytes();
    gpuScene.flushUploads(uploader, m_frameIndex);
    rebuildGeometryDescriptorSets();
    descriptorSetsDirty = false;
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    TRACE_EVENT("Render", "SceneUploaded", "RenderWorld")
        .text(std::format("scene on the GPU: {} draw slots, {} meshes, {} textures ({}) in {:.0f} ms", gpuInstances.size(), gpuScene.meshRanges().size(), gpuScene.textures().size(), formatUploadBytes(bytes), ms))
        .field("instances", (int64_t) gpuInstances.size())
        .field("meshes", (int64_t) gpuScene.meshRanges().size())
        .field("textures", (int64_t) gpuScene.textures().size())
        .field("bytes", (int64_t) bytes)
        .field("ms", ms);
}

auto Renderer::toSamplerDesc(const SamplerSettings& settings) -> RhiSamplerDesc {
    return {
        .mipmapMode = settings.nearestMip ? RhiMipmapMode::Nearest : RhiMipmapMode::Linear,
        .maxAnisotropy = settings.maxAnisotropy,
        .minLod = settings.minLod,
        .mipLodBias = settings.lodBias,
    };
}

auto Renderer::applySamplerSettings(const SamplerSettings& settings) -> void {
    materialSamplerSettings = settings;
    // The old sampler may be referenced by descriptor sets a frame in flight still binds.
    deletionQueue.defer(m_frameIndex, [dev = device, sampler = materialSampler] { dev->destroySampler(sampler); });
    materialSampler = device->createSampler(toSamplerDesc(settings));
    device->setDebugName(materialSampler, "sampler.material");
    rebuildGeometryDescriptorSets();
}

auto Renderer::rebuildGeometryDescriptorSets() -> void {
    using enum RhiDescriptorType;
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

    // The culling passes name the same instance buffer and mesh table, plus the draw-list and render-LOD buffers.
    instanceCullPass.rebuildDescriptors(device, drawLists, gpuScene, lodGroups, depthPyramid, deletionQueue, m_frameIndex);
    lodSelectPass.rebuildDescriptors(device, lodGroups, deletionQueue, m_frameIndex);

    // Tables come from applySceneChanges, the pool's buffers with the first mesh; nothing to point at before them.
    if (gpuInstances.empty() || gpuScene.materialBuffer() == nullptr || !gpuScene.poolReady()) {
        return;
    }

    // One set per frame slot: the slot's UBO and culling buffers plus the scene
    // tables and the geometry pool. Every texture-array element is written: the slot's texture or the fallback.
    auto imgCount = swapchain->imageCount();
    auto bindings = GeometryPass::descriptorBindings();
    geometryDescriptorPool = device->createDescriptorPool(imgCount, bindings);
    device->setDebugName(geometryDescriptorPool, "geometry.sets.pool");
    geometryDescriptorSets.assign(imgCount, nullptr);
    device->allocateDescriptorSets(geometryDescriptorPool, geometryPass.descriptorSetLayout(), geometryDescriptorSets);
    for (uint32_t i = 0; i < imgCount; i++) {
        device->setDebugName(geometryDescriptorSets[i], std::format("geometry.set.slot{}", i).c_str());
    }

    auto slots = gpuScene.textureSlots();
    std::vector<RhiDescriptorWrite> writes;
    writes.reserve(GpuScene::maxTextures + 4);
    for (uint32_t i = 0; i < imgCount; i++) {
        writes.clear();
        writes.push_back({
            .binding = 0,
            .type = UniformBuffer,
            .buffer = uniformBuffers[i],
            .bufferRange = sizeof(UniformBufferObject),
        });
        for (uint32_t t = 0; t < GpuScene::maxTextures; t++) {
            writes.push_back({
                .binding = 1,
                .arrayElement = t,
                .type = CombinedImageSampler,
                .texture = t < slots.size() ? slots[t] : fallbackTexture,
                .sampler = materialSampler,
            });
        }
        writes.push_back({
            .binding = 2,
            .type = StorageBuffer,
            .buffer = gpuScene.instanceBuffer(),
        });
        writes.push_back({
            .binding = 3,
            .type = StorageBuffer,
            .buffer = gpuScene.materialBuffer(),
        });
        auto cull = drawLists.slotBuffers(i);
        std::array<RhiBuffer*, 9> meshletBuffers = {
            gpuScene.vertexBuffer(),
            gpuScene.positionBuffer(),
            gpuScene.meshletBuffer(),
            gpuScene.meshletVertexBuffer(),
            gpuScene.meshletTriangleBuffer(),
            gpuScene.meshTableBuffer(),
            cull.workItems,
            cull.params,
            cull.meshletStats,
        };
        for (uint32_t b = 0; b < meshletBuffers.size(); b++) {
            writes.push_back({.binding = 4 + b, .type = StorageBuffer, .buffer = meshletBuffers[b]});
        }
        writes.push_back({.binding = 13, .type = CombinedImageSampler, .texture = depthPyramid.texture(), .sampler = depthPyramid.sampler()});
        writes.push_back({.binding = 14, .type = StorageBuffer, .buffer = cull.regionWork});
        writes.push_back({.binding = 15, .type = StorageBuffer, .buffer = lodGroups.stateBuffer()});
        device->updateDescriptorSet(geometryDescriptorSets[i], writes);
    }
}

// A view's triangles over its buckets; the camera is view 0, the cascades 1...
static auto trianglesIn(const DrawLists& drawLists, uint32_t view) -> uint64_t {
    uint64_t triangles = 0;
    for (uint32_t b = 0; b < DrawLists::bucketCount; b++) {
        triangles += drawLists.primitivesIn((view * DrawLists::bucketCount) + b);
    }
    return triangles;
}

static auto meshletCountsIn(const DrawLists& drawLists, uint32_t view) -> MeshletCounts {
    return {
        .tested = drawLists.meshletCount(view, DrawLists::meshletCounterTested),
        .frustumCulled = drawLists.meshletCount(view, DrawLists::meshletCounterFrustum),
        .coneCulled = drawLists.meshletCount(view, DrawLists::meshletCounterCone),
        .occluded = drawLists.meshletCount(view, DrawLists::meshletCounterOccluded),
        .drawn = drawLists.meshletCount(view, DrawLists::meshletCounterDrawn),
        .triangles = drawLists.meshletCount(view, DrawLists::meshletCounterTriangles),
    };
}

auto Renderer::cullResult() const -> CullResult {
    CullResult result = {
        .frame = drawLists.readbackFrame(),
        .instances = drawLists.instanceCount(),
        .cameraCulled = drawLists.cameraCulled(),
        .cameraInstanceTriangles = trianglesIn(drawLists, 0) + trianglesIn(drawLists, DrawLists::lateView),
        .cascadeCount = drawLists.cascadeCount(),
        .viewBits = drawLists.viewBits(),
        .regionOverflow = drawLists.regionOverflow(),
        .lodDropped = drawLists.lodDropped(),
        .lodFadingInstances = drawLists.fadingInstances(),
    };
    // The camera's meshlets over both phases.
    auto early = meshletCountsIn(drawLists, 0);
    auto late = meshletCountsIn(drawLists, DrawLists::lateView);
    result.cameraMeshlets = {
        .tested = early.tested + late.tested,
        .frustumCulled = early.frustumCulled + late.frustumCulled,
        .coneCulled = early.coneCulled + late.coneCulled,
        .occluded = early.occluded + late.occluded,
        .drawn = early.drawn + late.drawn,
        .triangles = early.triangles + late.triangles,
    };
    result.cameraTriangles = result.cameraMeshlets.triangles;
    result.occlusion = drawLists.occlusionEnabled();
    result.occlusionCullCamera = drawLists.occlusionCullCamera();
    for (uint32_t region = 0; region < DrawLists::regionCount; region++) {
        result.regionDraws.push_back(drawLists.drawsIn(region));
        result.regionMeshlets.push_back(drawLists.meshletsIn(region));
        result.regionTriangles.push_back(drawLists.primitivesIn(region));
    }
    result.occluded = drawLists.cameraOccluded();
    for (uint32_t b = 0; b < DrawLists::bucketCount; b++) {
        result.earlyDrawn += drawLists.drawsIn(b);
        result.lateDrawn += drawLists.drawsIn((DrawLists::lateView * DrawLists::bucketCount) + b);
    }
    result.cameraDrawn = result.earlyDrawn + result.lateDrawn;
    const auto& lod = lodGroups.latest();
    result.lodFrame = lod.frame;
    result.lodGroups = lod.totals.groups;
    result.lodPicked = lod.totals.picked;
    result.lodFallbacks = lod.totals.fallbacks;
    result.lodForced = lod.totals.forced;
    result.lodFading = lod.totals.fading;
    result.lodPopped = lod.totals.popped;
    result.lodFlags = lod.flags;
    result.lodHasStates = lod.hasStates || lod.groupCount == 0;
    result.lodEye = lod.eye;
    result.lodWindowArea = lod.windowArea;
    result.lodDrawn = lod.drawn;
    result.lodWanted = lod.wanted;
    result.lodFadeLevel = lod.fadeLevel;
    result.lodFade = lod.fade;
    result.lodMeasure = lod.measure;
    for (uint32_t c = 0; c < result.cascadeCount; c++) {
        result.cascadeCulled[c] = drawLists.cascadeCulled(c);
        result.cascadeDrawn[c] = drawLists.cascadeDrawn(c);
        result.cascadeInstanceTriangles[c] = trianglesIn(drawLists, 1 + c);
        result.cascadeMeshlets[c] = meshletCountsIn(drawLists, 1 + c);
        result.cascadeTriangles[c] = result.cascadeMeshlets[c].triangles;
    }
    result.sceneTriangles = sceneTriangles;
    result.sceneMeshlets = drawLists.liveMeshlets();
    return result;
}

auto Renderer::render(RenderSnapshot& snapshot) -> void {
    renderFrame(snapshot);
    for (auto* extension : extensions) {
        extension->endFrame(*this);
    }
}

auto Renderer::renderFrame(RenderSnapshot& snapshot) -> void {
    auto frame = ++m_frameIndex;
    if (snapshot.captureWatches.has_value()) {
        captureService.setWatches(std::move(*snapshot.captureWatches));
    }
    RenderFrame current = {
        .frame = frame,
        .slot = currentFrame,
    };
    for (auto* extension : extensions) {
        extension->beginFrame(*this, snapshot, current);
    }

    {
        PROFILE_ZONE("WaitFence");
        device->waitForFence(inflightFences[currentFrame]);
    }
    // This slot's previous frame is done, and so is everything submitted before it.
    deletionQueue.flush(slotFrame[currentFrame]);
    {
        PROFILE_ZONE("ReadGpuTimings");
        readGpuTimings(currentFrame);
    }
    {
        PROFILE_ZONE("ParseReadback");
        drawLists.parseReadback(currentFrame, slotFrame[currentFrame]);
        // Commands dropped past their region's end mean the CPU's bucket counts disagree with the cull shader's rule: draws are missing.
        if (auto overflow = drawLists.regionOverflow(); overflow != reportedRegionOverflow) {
            if (overflow > 0) {
                TRACE_WARNING("Render", "CullRegionOverflow", "DrawLists")
                    .text(std::format("{} draw commands dropped past their region's end: the CPU's bucket counts are short of the cull shader's buckets, so those draws are missing", overflow))
                    .field("dropped", (int64_t) overflow);
            }
            reportedRegionOverflow = overflow;
        }
        lodGroups.parseReadback(currentFrame, slotFrame[currentFrame]);
        // Meshlets that didn't fit the work items weren't drawn: grow past them before the next frame builds.
        if (auto overflow = drawLists.workOverflow(); overflow > 0) {
            auto before = drawLists.workCapacity();
            if (drawLists.ensureWorkCapacity(0, m_frameIndex)) {
                descriptorSetsDirty = true;
                TRACE_EVENT("Render", "MeshletWorkGrown", "cull.workitems")
                    .text(std::format("{} meshlets didn't fit the work items ({}): grown to {}", overflow, before, drawLists.workCapacity()))
                    .field("overflow", (int64_t) overflow)
                    .field("capacity", (int64_t) drawLists.workCapacity());
            } else if (!reportedWorkFull) {
                TRACE_WARNING("Render", "MeshletWorkFull", "cull.workitems")
                    .text(std::format("{} meshlets didn't fit the work items ({}), already the largest buffer the device makes: they aren't drawn", overflow, before))
                    .field("overflow", (int64_t) overflow)
                    .field("capacity", (int64_t) before);
                reportedWorkFull = true;
            }
        }
    }
    captureService.afterFence(currentFrame);
    for (auto* extension : extensions) {
        extension->afterFence(*this, currentFrame);
    }

    std::expected<uint32_t, RhiError> index;
    {
        PROFILE_ZONE("Acquire");
        index = swapchain->acquireNextImage(imageAvailableSemaphores[currentFrame]);
    }
    if (!index) {
        if (index.error() == RhiError::OutOfDate) {
            TRACE_EVENT("Render", "SwapchainRecreate", "swapchain").text("swapchain out of date at acquire; recreating").field("reason", "acquire_failed");
            if (!swapchain->recreate({.width = (uint32_t) snapshot.windowWidth, .height = (uint32_t) snapshot.windowHeight})) {
                TRACE_ERROR("Render", "SwapchainRecreateFailed", "swapchain").text("swapchain recreate failed after acquire");
            } else {
                recreateDepthTexture(swapchain->extent());
            }
            resourcePool.flush();
            currentFrame = 0;
        }
        return;
    }

    device->resetFence(inflightFences[currentFrame]);

    UniformBufferObject ubo = {
        .view = snapshot.viewMatrix,
        .proj = snapshot.projMatrix,
        .cullViewProj = snapshot.cullViewProj,
    };
    memcpy(uniformBuffersMapped[currentFrame], &ubo, sizeof(ubo));

    // Pipelines whose shaders arrived since the last frame, a few at a time; none change while the frame records.
    {
        PROFILE_ZONE("PipelineUpdate");
        pipelineRegistry.update(m_frameIndex);
    }

    // Build frame graph
    static const uint32_t buildZoneId = profile::registerName("BuildFrameGraph");
    profile::beginZone(buildZoneId);
    auto ext = swapchain->extent();
    frameGraph.reset();

    auto colorHandle = frameGraph.importTexture("backbuffer", swapchain->image(*index), {ext.width, ext.height, swapchain->colorFormat()});
    auto depthHandle = frameGraph.importTexture("depth", depthTexture, {ext.width, ext.height, depthFormat, RhiTextureUsage::DepthAttachment | RhiTextureUsage::Sampled});
    current.extent = ext;
    current.backbuffer = swapchain->image(*index);
    current.color = colorHandle;
    current.depth = depthHandle;

    // Textures uploaded in earlier frames take their slots; then this frame's scene uploads, before anything reads the tables.
    static const uint32_t sceneFrameZoneId = profile::registerName("GpuSceneBeginFrame");
    profile::beginZone(sceneFrameZoneId);
    if (gpuScene.beginFrame(m_frameIndex) || descriptorSetsDirty) {
        rebuildGeometryDescriptorSets();
        descriptorSetsDirty = false;
    }
    gpuScene.addUploadPass(frameGraph, m_frameIndex);
    profile::endZone();
    // Per-frame resources (UBOs, descriptor sets, dynamic vertex buffers) are owned by
    // the frame slot whose fence guards them, not by the swapchain image.
    auto imageIdx = currentFrame;
    auto instanceCount = (uint32_t) gpuInstances.size();

    // The lit path needs its pipelines, which come as their shaders arrive: until then the frame is the clear colour and the UI. The
    // instance records wait too, so their upload isn't lost to a frame that doesn't draw them.
    // Occlusion culling runs in two phases, skipped while culling is off: by default the geometry pass draws the first (what was visible
    // last frame), the pyramid is built from its depth, and a second geometry pass draws what the pyramid newly shows. With the depth
    // prepass on, the prepass runs the two phases instead and the geometry pass draws both behind its Equal test. While the culling camera
    // isn't the rendered one (a debug camera, a frozen frustum) the prepass's phases draw from the culling camera into a depth buffer of
    // their own, and the rendered camera gets no prepass: the geometry pass tests and writes depth itself.
    bool occlusion = snapshot.occlusion && snapshot.cullEnabled && depthPyramid.ready() && depthPrepass.ready();
    bool occlusionApart = occlusion && snapshot.cullCameraSeparate;
    bool prepass = snapshot.depthPrepass && !occlusionApart;
    bool occlusionInGeometry = occlusion && !prepass && !occlusionApart;
    bool litReady = instanceCullPass.ready() && lodSelectPass.ready() && shadowPass.ready() && geometryPass.ready() && (!prepass || depthPrepass.ready()) && lightingPass.ready() &&
                    aaPass.ready();
    if (!litReady) {
        addClearPass(frameGraph, colorHandle, ext);
    } else {
        static const uint32_t instanceUploadZoneId = profile::registerName("InstanceUpload");
        profile::beginZone(instanceUploadZoneId);
        auto instanceHandle = gpuScene.addInstanceUploadPasses(frameGraph, gpuInstances, currentFrame);
        profile::endZone();

        static const uint32_t shadowSetupZoneId = profile::registerName("ShadowSetup");
        profile::beginZone(shadowSetupZoneId);
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
        lighting.up = snapshot.worldUp;
        lighting.ambient = snapshot.ambient;
        lighting.cascadeCamera = snapshot.cascadeCamera;
        // The sky once its image is on the GPU: a texture gets its slot when its last level is copied.
        if (snapshot.sky && environment) {
            const auto& textures = gpuScene.textures();
            if (auto found = textures.find(environment->texture.index); found != textures.end() && found->second.slot != GpuScene::fallbackSlot) {
                lighting.sky = LightingInputs::Sky{
                    .texture = found->second.texture,
                    .radiance = environment->radiance,
                    .toImage = environment->toImage,
                    .irradiance = environment->irradiance,
                };
            }
        }
        debugSky = lighting.sky.has_value();
        if (picked != nullptr) {
            auto raw = glm::vec3(picked->worldTransform[2]);
            lighting.direction = glm::dot(raw, raw) > 1e-6f ? glm::normalize(raw) : snapshot.worldUp;
            lighting.radiance = picked->color * (picked->intensity * std::exp2(picked->exposure));
            lighting.shadowColor = picked->shadowColor;
        } else {
            lighting.direction = snapshot.worldUp;
        }
        debugHasSun = picked != nullptr;
        debugSun = lighting;

        // Cascades come fitted and culled from the main thread. A snapshot without them (no
        // cull ran yet) gets a single cascade fitted here from the scene bounds.
        std::array<ShadowCascade, maxShadowCascades> fallbackCascades;
        std::span<const ShadowCascade> cascades(snapshot.cascades.data(), snapshot.cascadeCount);
        if (snapshot.cascadeCount == 0) {
            ShadowCascadeSettings single = snapshot.shadowSettings;
            single.count = 1;
            auto count = fitShadowCascades(single, snapshot.viewMatrix, snapshot.projMatrix, projectionNear, lighting.direction, snapshot.worldUp, sceneBounds, fallbackCascades);
            cascades = std::span<const ShadowCascade>(fallbackCascades.data(), count);
        }
        auto atlasExtent = shadowAtlasExtent(snapshot.shadowSettings);
        if (cascades.size() <= 1) {
            atlasExtent = {std::max(64u, snapshot.shadowSettings.tileSize), std::max(64u, snapshot.shadowSettings.tileSize)};
        }
        debugShadowExtent = atlasExtent;

        auto invViewProj = glm::inverse(snapshot.projMatrix * snapshot.viewMatrix);

        profile::endZone(); // ShadowSetup
        // Render LOD picks each group's level from the live main camera (not a frozen culling frustum, nor a debug camera rendered from):
        // its position, and the view window's area at depth 1 from the projection (2 / |proj[0][0]| wide, 2 / |proj[1][1]| high), as the
        // view's heuristic measures.
        auto lodHandles = lodGroups.import(frameGraph, currentFrame);
        LodSelectPass::Camera lodCamera{
            .eye = snapshot.lodEye,
            .windowArea = 4.0f / std::max(1e-12f, std::abs(snapshot.projMatrix[0][0] * snapshot.projMatrix[1][1])),
        };
        lodSelectPass.addPasses(frameGraph, lodHandles, lodCamera, snapshot.lodHysteresis, snapshot.lodFlashFrames, lodGroups.groupCount(), currentFrame);
        // GPU culling writes the indirect commands for every view; the passes below read them. Every pass binding the geometry set reads
        // the depth pyramid.
        DrawLists::Occlusion occlusionParams = {.enabled = occlusion, .cullCamera = occlusionApart, .hzbWidth = depthPyramid.width(), .hzbHeight = depthPyramid.height()};
        auto cullGroups = drawLists.writeParams(currentFrame, instanceCount, snapshot.cullViewProj, cascades, snapshot.cullEnabled, occlusionParams);
        auto drawHandles = drawLists.import(frameGraph, currentFrame);
        drawHandles.lodStates = lodHandles.states;
        auto hzbHandle = depthPyramid.import(frameGraph);
        instanceCullPass.addPasses(frameGraph, drawLists, drawHandles, lodHandles, instanceHandle, hzbHandle, cullGroups, currentFrame);
        if (snapshot.sampler != materialSamplerSettings) {
            applySamplerSettings(snapshot.sampler);
        }
        auto* geometrySet = imageIdx < geometryDescriptorSets.size() ? geometryDescriptorSets[imageIdx] : nullptr;
        const auto& shadowData = shadowPass.addPass(frameGraph, atlasExtent, depthFormat, cascades, gpuInstances, instanceHandle, drawLists, drawHandles, gpuScene, geometrySet, hzbHandle);

        debugCulledInstances = drawLists.cameraCulled();
        // Occlusion's depth: the rendered camera's, or the culling camera's own while that isn't the rendered one.
        auto occlusionDepth = depthHandle;
        auto occlusionSource = DepthPyramid::Source::View;
        if (occlusionApart) {
            occlusionDepth = frameGraph.importTexture("depth.cull", cullDepthTexture, {ext.width, ext.height, depthFormat, RhiTextureUsage::DepthAttachment | RhiTextureUsage::Sampled});
            occlusionSource = DepthPyramid::Source::Culling;
        }
        if (prepass || occlusionApart) {
            depthPrepass.addPass(frameGraph, occlusionDepth, ext, gpuInstances, instanceHandle, drawLists, drawHandles, gpuScene, geometrySet, hzbHandle, false, occlusionApart);
        }
        // Occlusion's second phase: the pyramid from the first phase's depth, every frustum-visible instance against it, and the depth of
        // the ones that show and weren't drawn. Then the pyramid again from the whole depth: the geometry pass tests every meshlet of both
        // phases against it, so it shades only meshlets that show.
        if (occlusion && !occlusionInGeometry) {
            depthPyramid.addBuildPass(frameGraph, "HzbBuild", occlusionSource, occlusionDepth, hzbHandle);
            instanceCullPass.addLatePasses(frameGraph, drawHandles, instanceHandle, hzbHandle, cullGroups, currentFrame);
            depthPrepass.addPass(frameGraph, occlusionDepth, ext, gpuInstances, instanceHandle, drawLists, drawHandles, gpuScene, geometrySet, hzbHandle, true, occlusionApart);
            depthPyramid.addBuildPass(frameGraph, "HzbBuildFinal", occlusionSource, occlusionDepth, hzbHandle);
        }
        // Occlusion in the geometry pass: the first phase draws the G-buffer, the pyramid comes from its depth, the second phase draws what
        // the pyramid newly shows, its meshlets tested against it.
        const GeometryPassData* geometryResult = nullptr;
        if (occlusionInGeometry) {
            const auto& firstPhase = geometryPass.addPass(frameGraph, depthHandle, ext, gpuInstances, instanceHandle, drawLists, drawHandles, gpuScene, geometrySet, false, hzbHandle, occlusion, GeometryPhase::First, nullptr);
            depthPyramid.addBuildPass(frameGraph, "HzbBuild", DepthPyramid::Source::View, firstPhase.depth, hzbHandle);
            instanceCullPass.addLatePasses(frameGraph, drawHandles, instanceHandle, hzbHandle, cullGroups, currentFrame);
            geometryResult = &geometryPass.addPass(frameGraph, depthHandle, ext, gpuInstances, instanceHandle, drawLists, drawHandles, gpuScene, geometrySet, false, hzbHandle, occlusion, GeometryPhase::Second, &firstPhase);
        } else {
            geometryResult = &geometryPass.addPass(frameGraph, depthHandle, ext, gpuInstances, instanceHandle, drawLists, drawHandles, gpuScene, geometrySet, prepass, hzbHandle, occlusion, GeometryPhase::Both, nullptr);
        }
        const auto& geomData = *geometryResult;

        drawLists.addReadbackPass(frameGraph, drawHandles, currentFrame, snapshot.readVisibility, drawLogEnabled, m_frameIndex);
        lodGroups.addReadbackPass(frameGraph, lodHandles, currentFrame, snapshot.readLodStates, lodCamera.eye, lodCamera.windowArea);

        // The depth preview's far end: the camera has no far plane, so the farthest corner of the scene.
        float previewFar = projectionNear * 10.0f;
        if (sceneBounds.valid()) {
            auto eye = glm::vec3(glm::inverse(snapshot.viewMatrix)[3]);
            for (int i = 0; i < 8; i++) {
                glm::vec3 corner = {
                    (i & 1) != 0 ? sceneBounds.max.x : sceneBounds.min.x,
                    (i & 2) != 0 ? sceneBounds.max.y : sceneBounds.min.y,
                    (i & 4) != 0 ? sceneBounds.max.z : sceneBounds.min.z,
                };
                previewFar = std::max(previewFar, glm::length(corner - eye));
            }
        }
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
            occlusion ? hzbHandle : FgTextureHandle{},
            snapshot.hzbLevel,
            snapshot.showBufferOverlay,
            snapshot.showShadowOverlay,
            snapshot.lodPopFlash,
            snapshot.lodColorByDepth,
            invViewProj,
            shadowSampler,
            cascades,
            snapshot.shadowSettings.pcf,
            atlasExtent,
            previewFar,
            fallbackTexture);

        // AA filters only the lit scene. Its output is blitted to the backbuffer and the
        // overlays (debug lines, gizmos, UI) draw on the backbuffer afterwards, so they are
        // never filtered and sceneColorAA holds nothing but the AA result.
        const auto& aaData = aaPass.addPass(frameGraph, lightData.sceneColor, ext, imageIdx, textureSampler, snapshot.antiAliasing);

        // A debug view replaces the lit image (the streaming view lays its colours over it); it is
        // drawn after everything the geometry pass wrote and never anti-aliased (AA would blend IDs).
        auto finalImage = aaData.sceneColorAA;
        // Its pipelines come on first use: until they are ready the lit image shows.
        if (snapshot.debugView != DebugView::None && debugViewPass.available() && debugViewPass.ready()) {
            const auto& debugViewData = debugViewPass.addPass(frameGraph, snapshot.debugView, geomData.depth, ext, imageIdx, instanceHandle, drawLists, drawHandles, gpuScene, geometrySet, hzbHandle, occlusion, snapshot.streamingStates, lightData.sceneColor);
            finalImage = debugViewData.color;
        }
        addBlitPass(frameGraph, "BlitToBackbuffer", finalImage, colorHandle, ext, ext);
    }

    if (debugRenderer.ready()) {
        debugRenderer.addPass(frameGraph, colorHandle, depthHandle, ext, snapshot.debugData, imageIdx);
    }

    // The extensions' overlays: the tools' gizmos, the UI.
    for (auto* extension : extensions) {
        extension->addOverlayPasses(*this, snapshot, current);
    }

    addPresentPass(frameGraph, colorHandle);

    profile::zoneValue(instanceCount);
    profile::endZone(); // BuildFrameGraph
    frameGraph.setDrawLogEnabled(drawLogEnabled);
    captureService.setStaticBuffer("gpuscene.meshtable", gpuScene.meshTableBuffer(), gpuScene.meshTableBytes());
    captureService.setStaticBuffer("gpuscene.materials", gpuScene.materialBuffer(), gpuScene.materialTableBytes());
    if (gpuScene.meshletBuffer() != nullptr) {
        captureService.setStaticBuffer("gpuscene.meshlets", gpuScene.meshletBuffer(), gpuScene.meshletBytes());
    }
    frameGraph.setCaptureRequests(captureService.requestsForFrame(currentFrame, frame));
    {
        PROFILE_ZONE("Compile");
        frameGraph.compile();
    }

    // Command buffer and fence belong to the frame slot; the render-finished semaphore
    // belongs to the swapchain image, since present consumes it per image.
    auto* cmd = cmdBuffers[currentFrame];
    {
        PROFILE_ZONE("Record");
        PROFILE_ZONE_VALUE(frameGraph.passCount() - frameGraph.culledCount());
        for (auto* extension : extensions) {
            extension->beforeExecute(*this, cmd, current);
        }
        cmd->reset();
        cmd->begin();
        frameGraph.execute(cmd);
        // The extensions' copies after the frame (the frame debugger, the inspector, dumps, screenshots), the command log first off.
        for (auto* extension : extensions) {
            extension->afterExecute(*this, cmd, current);
        }
        gpuScene.afterExecute(frameGraph);
        lodGroups.afterExecute(frameGraph);
        drawLists.afterExecute(frameGraph);
        captureService.recordStatic(cmd, currentFrame, frame);
        cmd->end();
    }
    slotDrawLogs[currentFrame] = frameGraph.drawLog();
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

    for (auto* extension : extensions) {
        extension->afterSubmit(*this, current);
    }
    std::expected<void, RhiError> presented;
    {
        PROFILE_ZONE("Present");
        presented = device->present(swapchain, renderFinishedSemaphores[*index], *index);
    }
    if (!presented) {
        TRACE_EVENT("Render", "SwapchainRecreate", "swapchain").text("swapchain out of date at present; recreating").field("reason", "present_failed");
        if (!swapchain->recreate({.width = (uint32_t) snapshot.windowWidth, .height = (uint32_t) snapshot.windowHeight})) {
            TRACE_ERROR("Render", "SwapchainRecreateFailed", "swapchain").text("swapchain recreate failed after present");
        } else {
            recreateDepthTexture(swapchain->extent());
        }
        resourcePool.flush();
        currentFrame = 0;
        return;
    }

    if (!firstFramePresented) {
        firstFramePresented = true;
        TRACE_EVENT("Render", "FirstFramePresented", "frame")
            .text(std::format("first frame presented (frame {}), {:.0f} ms after start", frame, trace::msSinceStart()))
            .field("frame", (int64_t) frame)
            .field("since_start_ms", trace::msSinceStart());
    }
    currentFrame = (currentFrame + 1) % swapchain->imageCount();
}

auto Renderer::waitForFrame() -> void {
    device->waitForFence(inflightFences[currentFrame]);
}

auto Renderer::state() const -> RendererState {
    return {
        .frame = m_frameIndex,
        .slot = currentFrame,
        .device = device,
        .swapchainExtent = swapchain->extent(),
        .swapchainFormat = swapchain->colorFormat(),
        .swapchainImages = swapchain->imageCount(),
        .gpuScene = &gpuScene,
        .instances = gpuInstances,
        .culledInstances = debugCulledInstances,
        .lightCount = (uint32_t) lights.size(),
        .hasSun = debugHasSun,
        .sun = debugSun,
        .skyDrawn = debugSky,
        .environment = environment ? &*environment : nullptr,
        .shadowAtlasExtent = debugShadowExtent,
        .resourcePool = &resourcePool,
        .drawLog = lastDrawLog,
        .passGpuTimes = lastGpuTimes,
        .gpuFrameMs = lastGpuFrameMs,
        .gpuFrame = lastGpuFrame,
        .gpuZones = gpuZoneScratch,
    };
}

auto Renderer::destroy() -> void {
    device->waitIdle();
    deletionQueue.flushAll();
    uploader.destroy();

    captureService.destroy();
    for (auto* extension : extensions) {
        extension->destroy();
    }

    if (geometryDescriptorPool) {
        device->freeDescriptorSets(geometryDescriptorPool, geometryDescriptorSets);
        device->destroyDescriptorPool(geometryDescriptorPool);
    }

    gpuScene.destroy();
    drawLists.destroy();
    lodGroups.destroy();

    pipelineRegistry.destroy();
    instanceCullPass.destroy(device);
    depthPyramid.destroy();
    lodSelectPass.destroy(device);
    geometryPass.destroy(device);
    lightingPass.destroy(device);
    aaPass.destroy(device);
    debugViewPass.destroy(device);
    debugRenderer.destroy(device);

    for (uint32_t i = 0; i < swapchain->imageCount(); i++) {
        device->unmapBuffer(uniformBuffers[i]);
        device->destroyBuffer(uniformBuffers[i]);
    }

    device->destroySampler(textureSampler);
    device->destroySampler(shadowSampler);
    device->destroySampler(materialSampler);
    device->destroyTexture(fallbackTexture);
    device->destroyTexture(pendingTexture);
    device->destroyTexture(depthTexture);
    depthTexture = nullptr;
    device->destroyTexture(cullDepthTexture);
    cullDepthTexture = nullptr;

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

namespace renderconfig {

config::Bool antiAliasing{"render.antialiasing", true, "FXAA on the lit image; off shows it as the passes wrote it (a LOD fade's dither unblurred)."};
config::Bool depthPrepass{"render.prepass", false, "A depth-only pass before the geometry pass; off by default, as it costs more than it saves without heavy overdraw."};
config::Float samplerAnisotropy{"render.sampler.anisotropy", 8.0, "The material sampler's maximum anisotropy; 0 turns it off.", {.min = 0.0, .max = 16.0}};
config::Float samplerLodBias{"render.sampler.lodBias", 0.0, "Added to the mip level the material sampler picks; positive is blurrier.", {.min = -16.0, .max = 16.0}};
config::Float samplerMinLod{"render.sampler.minLod", 0.0, "The finest mip level the material sampler may use; raising it forces coarser levels.", {.min = 0.0, .max = 16.0}};
config::Bool samplerNearestMip{"render.sampler.nearestMip", false, "The nearest mip level instead of a blend of the two nearest."};

auto samplerSettings() -> SamplerSettings {
    return {
        .maxAnisotropy = (float) samplerAnisotropy.get(),
        .lodBias = (float) samplerLodBias.get(),
        .minLod = (float) samplerMinLod.get(),
        .nearestMip = samplerNearestMip.get(),
    };
}

auto setSamplerSettings(const SamplerSettings& settings) -> void {
    (void) samplerAnisotropy.set(settings.maxAnisotropy);
    (void) samplerLodBias.set(settings.lodBias);
    (void) samplerMinLod.set(settings.minLod);
    samplerNearestMip.set(settings.nearestMip);
}

} // namespace renderconfig
