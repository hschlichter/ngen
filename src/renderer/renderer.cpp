#include "renderer.h"

#include "blitpass.h"
#include "imguibackend.h"
#include "instanceuploadpass.h"
#include "material.h"
#include "mesh.h"
#include "mipchain.h"
#include "observationmacros.h"
#include "presentpass.h"
#include "profile.h"
#include "rendersnapshot.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "rhiswapchain.h"
#include "screenshot.h"
#include "shadowpass.h"

#include <algorithm>
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
    // Shadow compare sampler: linear so the hardware compare is bilinear PCF, clamped so a
    // tile never reads its neighbour, LessOrEqual on the biased reference depth.
    shadowSampler = device->createSampler({
        .addressU = RhiAddressMode::ClampToEdge,
        .addressV = RhiAddressMode::ClampToEdge,
        .addressW = RhiAddressMode::ClampToEdge,
        .compareEnable = true,
        .compareOp = RhiCompareOp::LessOrEqual,
    });
    // Material sampler: trilinear with anisotropy, the textures carry full mip chains and
    // Sponza floors are seen at grazing angles. Rebuilt when the settings change.
    materialSampler = device->createSampler(toSamplerDesc(materialSamplerSettings));

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
    debugShadowExtent = shadowExtent;
    if (!shadowPass.init(device, shadowExtent, depthFmt)) {
        return std::unexpected(1);
    }
    if (!geometryPass.init(device, ext, depthFmt)) {
        return std::unexpected(1);
    }
    if (!depthPrepass.init(device, depthFmt, geometryPass.descriptorSetLayout())) {
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

    // Scene GPU tables; the instance buffer exists from here on, grown by uploadRenderWorld.
    gpuScene.init(device, imgCount, &deletionQueue);
    shadowPass.bindInstanceBuffer(device, gpuScene.instanceBuffer(), deletionQueue, m_frameIndex);
    boundInstanceGeneration = gpuScene.instanceGeneration();

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
    slotDrawLogs.resize(imgCount);
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

auto Renderer::buildRenderDebugSnapshot() const -> RenderDebugSnapshot {
    RenderDebugSnapshot snap;
    snap.frameIndex = m_frameIndex;
    snap.limits = device->limits();
    snap.validation = validationEnabled;
    snap.swapchainExtent = swapchain->extent();
    snap.swapchainFormat = swapchain->colorFormat();
    snap.swapchainImages = swapchain->imageCount();
    snap.currentSlot = currentFrame;

    snap.instanceCount = (uint32_t) gpuInstances.size();
    snap.culledInstances = debugCulledInstances;
    std::unordered_map<uint32_t, uint32_t> instancesPerMesh;
    for (const auto& inst : gpuInstances) {
        instancesPerMesh[inst.mesh.index]++;
        if (inst.primFirst) {
            snap.primFirstInstances++;
        }
    }
    snap.meshes.reserve(gpuScene.meshRanges().size());
    for (const auto& [index, cached] : gpuScene.meshRanges()) {
        snap.meshes.push_back({
            .meshIndex = index,
            .vertexCount = cached.vertexCount,
            .indexCount = cached.indexCount,
            .vertexBytes = cached.vertexBytes,
            .indexBytes = cached.indexBytes,
            .instances = instancesPerMesh[index],
        });
    }
    std::unordered_map<uint32_t, bool> materials;
    for (const auto& inst : gpuInstances) {
        if (inst.material) {
            materials[inst.material.index] = textureCache.contains(inst.material.index);
        }
    }
    snap.materialCount = (uint32_t) materials.size();
    for (const auto& [index, textured] : materials) {
        if (textured) {
            snap.materialsWithTexture++;
        }
    }
    snap.textures.reserve(textureCache.size());
    for (const auto& [index, cached] : textureCache) {
        snap.textures.push_back({
            .materialIndex = index,
            .width = cached.width,
            .height = cached.height,
            .mipLevels = cached.mipLevels,
            .format = cached.format,
            .bytes = cached.bytes,
        });
    }
    snap.inspect = textureInspectResult;
    snap.lightCount = (uint32_t) lights.size();
    snap.hasSun = debugHasSun;
    snap.sunDirection = debugSun.direction;
    snap.sunRadiance = debugSun.radiance;
    snap.sunShadowColor = debugSun.shadowColor;
    snap.shadowMapExtent = debugShadowExtent;

    auto fg = buildFrameGraphDebugSnapshot();
    snap.passes.reserve(fg.executionOrder.size());
    for (auto passIdx : fg.executionOrder) {
        const auto& pass = fg.passes[passIdx];
        snap.passes.push_back(pass);
        snap.frameTotals.draws += pass.stats.draws;
        snap.frameTotals.dispatches += pass.stats.dispatches;
        snap.frameTotals.barriers += pass.stats.barriers;
        snap.frameTotals.pipelineBinds += pass.stats.pipelineBinds;
        snap.frameTotals.bufferBinds += pass.stats.bufferBinds;
        snap.frameTotals.descriptorBinds += pass.stats.descriptorBinds;
        snap.frameTotals.copies += pass.stats.copies;
        snap.frameTotals.primitives += pass.stats.primitives;
    }
    resourcePool.forEach([&](const ResourcePoolKey& key, bool inUse) {
        snap.poolTextures.push_back({.width = key.width, .height = key.height, .format = key.format, .inUse = inUse});
    });
    snap.poolAllocationsTotal = resourcePool.stats().allocationsTotal;
    snap.draws = lastDrawLog;
    snap.drawTiming = drawTimingRequest;
    return snap;
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

    // Per-draw zones are named "Draw" and were recorded in the same order as the timed
    // entries of the slot's draw log.
    auto& drawLog = slotDrawLogs[slot];
    size_t nextTimed = 0;
    for (const auto& zone : gpuZoneScratch) {
        if (std::strcmp(zone.name, "Draw") != 0) {
            continue;
        }
        while (nextTimed < drawLog.size() && !drawLog[nextTimed].timed) {
            nextTimed++;
        }
        if (nextTimed >= drawLog.size()) {
            break;
        }
        drawLog[nextTimed].gpuMs = (double) (zone.endNs - zone.startNs) * 1e-6;
        nextTimed++;
    }
    lastDrawLog = drawLog;

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

    std::vector<uint32_t> primOfInstance(world.meshInstances.size(), 0);
    for (const auto& [prim, range] : world.primToInstance) {
        for (uint32_t i = range.first; i < range.first + range.count && i < primOfInstance.size(); i++) {
            primOfInstance[i] = prim;
        }
    }
    sceneBounds = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
    for (const auto& inst : world.meshInstances) {
        if (inst.worldBounds.valid()) {
            sceneBounds.min = glm::min(sceneBounds.min, inst.worldBounds.min);
            sceneBounds.max = glm::max(sceneBounds.max, inst.worldBounds.max);
        }
    }
    bool sizeChanged = gpuInstances.size() != world.meshInstances.size();
    auto instanceCount = (uint32_t) world.meshInstances.size();
    uint32_t changedFirst = sizeChanged ? 0 : instanceCount;
    uint32_t changedEnd = sizeChanged ? instanceCount : 0;
    gpuInstances.resize(world.meshInstances.size());
    for (size_t m = 0; m < world.meshInstances.size(); m++) {
        const auto& inst = world.meshInstances[m];
        if (!sizeChanged && gpuInstances[m].transform != inst.worldTransform) {
            changedFirst = std::min(changedFirst, (uint32_t) m);
            changedEnd = std::max(changedEnd, (uint32_t) m + 1);
        }
        gpuInstances[m] = {
            .mesh = inst.mesh,
            .material = inst.material,
            .prim = primOfInstance[m],
            .transform = inst.worldTransform,
            .indexOffset = inst.indexOffset,
            .indexCount = inst.indexCount,
            .primFirst = inst.primFirst,
            .doubleSided = inst.doubleSided,
        };
    }
    // A grown instance buffer is a new buffer: every descriptor set naming it is rewritten.
    gpuScene.updateInstances(gpuInstances, changedFirst, changedEnd, m_frameIndex);
    bool instanceBufferReplaced = gpuScene.instanceGeneration() != boundInstanceGeneration;
    if (instanceBufferReplaced) {
        shadowPass.bindInstanceBuffer(device, gpuScene.instanceBuffer(), deletionQueue, m_frameIndex);
        boundInstanceGeneration = gpuScene.instanceGeneration();
    }

    if (!geometryChanged) {
        if (instanceBufferReplaced) {
            rebuildGeometryDescriptorSets("instance_buffer");
        }
        return;
    }

    gpuScene.rebuildGeometry(gpuInstances, meshLib, uploader, m_frameIndex);

    // Old textures may still be referenced by frames in flight; destroy them once
    // the frame that last used them has completed.
    for (auto& [idx, cached] : textureCache) {
        deletionQueue.deferTexture(m_frameIndex, cached.texture);
    }
    textureCache.clear();

    uploader.begin();
    for (const auto& inst : world.meshInstances) {
        if (inst.material && !textureCache.contains(inst.material.index)) {
            const auto* matData = matLib.get(inst.material);
            if (matData && !matData->texPixels.empty()) {
                PROFILE_ZONE("UploadTexture");
                PROFILE_ZONE_VALUE((uint64_t) matData->texWidth * (uint64_t) matData->texHeight * 4);
                RhiTextureDesc texDesc = {
                    .width = (uint32_t) matData->texWidth,
                    .height = (uint32_t) matData->texHeight,
                    .format = RhiFormat::R8G8B8A8_SRGB,
                    // TransferSrc for the texture inspector's blits and level dumps.
                    .usage = RhiTextureUsage::Sampled | RhiTextureUsage::TransferDst | RhiTextureUsage::TransferSrc,
                    .mipLevels = mipLevelCount((uint32_t) matData->texWidth, (uint32_t) matData->texHeight),
                };
                auto pixelCount = (size_t) matData->texWidth * (size_t) matData->texHeight * 4;
                auto level0 = std::span(matData->texPixels).first(pixelCount);
                std::vector<std::byte> packed;
                {
                    PROFILE_ZONE("BuildMips");
                    auto chain = buildMipChain(texDesc.width, texDesc.height, level0, true);
                    packed = packMipChain(level0, chain);
                }
                textureCache[inst.material.index] = {
                    .texture = uploader.uploadTexture(texDesc, packed),
                    .width = texDesc.width,
                    .height = texDesc.height,
                    .mipLevels = texDesc.mipLevels,
                    .bytes = packed.size(),
                    .format = texDesc.format,
                };

                OBS_EVENT("Render", "TextureUploaded", "Texture")
                    .field("width", (int64_t) matData->texWidth)
                    .field("height", (int64_t) matData->texHeight)
                    .field("mips", (int64_t) texDesc.mipLevels)
                    .field("bytes", (int64_t) packed.size());
            }
        }
    }

    {
        PROFILE_ZONE("UploadSubmit");
        uploader.end();
    }

    gpuScene.rebuildMaterials(gpuInstances, textureCache, fallbackTexture, uploader, m_frameIndex);
    rebuildGeometryDescriptorSets("geometry");
}

// Preview extent for the inspector: the level scaled to fit a 256 box, up or down, so a
// 4x4 level shows as blocks instead of a dot.
static auto inspectPreviewExtent(uint32_t w, uint32_t h) -> RhiExtent2D {
    constexpr uint32_t box = 256;
    auto maxSide = std::max(w, h);
    if (maxSide == 0) {
        return {box, box};
    }
    return {std::max(1u, (w * box) / maxSide), std::max(1u, (h * box) / maxSide)};
}

auto Renderer::releaseTexturePreview() -> void {
    if (texturePreview.imguiId != 0) {
        deletionQueue.defer(m_frameIndex, [ui = editorUI, id = texturePreview.imguiId] { ui->unregisterTexture(id); });
    }
    if (texturePreview.texture != nullptr) {
        deletionQueue.deferTexture(m_frameIndex, texturePreview.texture);
    }
    texturePreview = {};
}

auto Renderer::recordTextureInspect(RhiCommandBuffer* cmd) -> void {
    textureInspectResult = {};
    if (!textureInspectRequest.enabled || editorUI == nullptr) {
        if (texturePreview.texture != nullptr) {
            releaseTexturePreview();
        }
        return;
    }
    auto texIt = textureCache.find(textureInspectRequest.material);
    if (texIt == textureCache.end()) {
        return;
    }
    const auto& cached = texIt->second;
    auto level = std::min(textureInspectRequest.level, cached.mipLevels - 1);
    uint32_t levelW = std::max(1u, cached.width >> level);
    uint32_t levelH = std::max(1u, cached.height >> level);
    auto ext = inspectPreviewExtent(levelW, levelH);

    if (texturePreview.texture == nullptr || texturePreview.width != ext.width || texturePreview.height != ext.height) {
        releaseTexturePreview();
        RhiTextureDesc desc = {
            .width = ext.width,
            .height = ext.height,
            .format = cached.format,
            .usage = RhiTextureUsage::Sampled | RhiTextureUsage::TransferDst,
        };
        texturePreview.texture = device->createTexture(desc);
        texturePreview.width = ext.width;
        texturePreview.height = ext.height;
        if (texturePreview.texture != nullptr) {
            texturePreview.imguiId = editorUI->registerTexture(texturePreview.texture, textureSampler);
        }
    }
    if (texturePreview.texture == nullptr) {
        return;
    }

    auto dstStart = texturePreview.everCaptured ? RhiTextureState::ShaderReadOnly : RhiTextureState::Undefined;
    std::array<RhiTextureBarrierDesc, 2> pre = {{
        {.texture = cached.texture, .oldState = RhiTextureState::ShaderReadOnly, .newState = RhiTextureState::TransferSrc},
        {.texture = texturePreview.texture, .oldState = dstStart, .newState = RhiTextureState::TransferDst},
    }};
    cmd->pipelineBarrier(pre);
    // Nearest when magnifying so texels stay visible as blocks; linear when shrinking.
    auto filter = ext.width > levelW ? RhiFilter::Nearest : RhiFilter::Linear;
    cmd->blitTexture(cached.texture, texturePreview.texture, {.mipLevel = level, .extent = {levelW, levelH}}, {.mipLevel = 0, .extent = ext}, filter);
    std::array<RhiTextureBarrierDesc, 2> post = {{
        {.texture = cached.texture, .oldState = RhiTextureState::TransferSrc, .newState = RhiTextureState::ShaderReadOnly},
        {.texture = texturePreview.texture, .oldState = RhiTextureState::TransferDst, .newState = RhiTextureState::ShaderReadOnly},
    }};
    cmd->pipelineBarrier(post);
    texturePreview.everCaptured = true;

    textureInspectResult = {
        .valid = true,
        .material = textureInspectRequest.material,
        .level = level,
        .mipLevels = cached.mipLevels,
        .levelWidth = levelW,
        .levelHeight = levelH,
        .levelBytes = (uint64_t) levelW * levelH * 4,
        .previewTextureId = texturePreview.imguiId,
        .previewWidth = ext.width,
        .previewHeight = ext.height,
    };
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
    rebuildGeometryDescriptorSets("sampler");
    OBS_EVENT("Render", "SamplerSettings", "material")
        .field("anisotropy", (double) settings.maxAnisotropy)
        .field("lod_bias", (double) settings.lodBias)
        .field("min_lod", (double) settings.minLod)
        .field("nearest_mip", settings.nearestMip);
}

auto Renderer::rebuildGeometryDescriptorSets(const char* reason) -> void {
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

    // Tables come from uploadRenderWorld; nothing to point at before the first one.
    if (gpuInstances.empty() || gpuScene.materialBuffer() == nullptr) {
        return;
    }

    // One set per frame slot (docs/plan_bindless_materials.md): the slot's UBO plus the scene
    // tables. Every texture-array element is written: the slot's texture or the fallback.
    auto imgCount = swapchain->imageCount();
    auto bindings = GeometryPass::descriptorBindings();
    geometryDescriptorPool = device->createDescriptorPool(imgCount, bindings);
    geometryDescriptorSets.assign(imgCount, nullptr);
    device->allocateDescriptorSets(geometryDescriptorPool, geometryPass.descriptorSetLayout(), geometryDescriptorSets);

    auto slots = gpuScene.textureSlots();
    std::vector<RhiDescriptorWrite> writes;
    writes.reserve(GpuScene::maxTextures + 3);
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
        device->updateDescriptorSet(geometryDescriptorSets[i], writes);
    }
    OBS_EVENT("Render", "GeometryDescriptorsRebuilt", "descriptors").field("sets", (int64_t) imgCount).field("reason", reason);
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

    auto instanceHandle = gpuScene.addUploadPasses(frameGraph, gpuInstances, currentFrame);

    // Per-frame resources (UBOs, descriptor sets, dynamic vertex buffers) are owned by
    // the frame slot whose fence guards them, not by the swapchain image.
    auto imageIdx = currentFrame;
    auto instanceCount = (uint32_t) gpuInstances.size();

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
        auto count = fitShadowCascades(single, snapshot.viewMatrix, snapshot.projMatrix, 0.1f, 3000.0f, lighting.direction, snapshot.worldUp, sceneBounds, fallbackCascades);
        cascades = std::span<const ShadowCascade>(fallbackCascades.data(), count);
    }
    auto atlasExtent = shadowAtlasExtent(snapshot.shadowSettings);
    if (cascades.size() <= 1) {
        atlasExtent = {std::max(64u, snapshot.shadowSettings.tileSize), std::max(64u, snapshot.shadowSettings.tileSize)};
    }
    debugShadowExtent = atlasExtent;
    debugCascadeCount = (uint32_t) cascades.size();
    debugShadowCulled = 0;
    for (auto culled : snapshot.shadowCulled) {
        debugShadowCulled += culled;
    }

    auto invViewProj = glm::inverse(snapshot.projMatrix * snapshot.viewMatrix);

    profile::endZone(); // ShadowSetup
    const auto& shadowData = shadowPass.addPass(frameGraph, atlasExtent, depthFormat, cascades, snapshot.shadowVisible, gpuInstances, instanceHandle, gpuScene);

    debugCulledInstances = snapshot.culledInstances;
    if (snapshot.sampler != materialSamplerSettings) {
        applySamplerSettings(snapshot.sampler);
    }
    auto* geometrySet = imageIdx < geometryDescriptorSets.size() ? geometryDescriptorSets[imageIdx] : nullptr;
    if (snapshot.depthPrepass) {
        depthPrepass.addPass(frameGraph, depthHandle, ext, gpuInstances, instanceHandle, snapshot.visible, gpuScene, geometrySet);
    }
    const auto& geomData = geometryPass.addPass(frameGraph, depthHandle, ext, instanceCount, gpuInstances, instanceHandle, snapshot.visible, gpuScene, geometrySet, snapshot.depthPrepass);

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
        shadowSampler,
        cascades,
        snapshot.shadowSettings.pcf,
        atlasExtent);

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
    frameGraph.setDrawLogEnabled(renderDebugEnabled);
    frameGraph.setDrawTiming(renderDebugEnabled ? drawTimingRequest : FgDrawTimingRequest{});
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
    // Screenshot: read the presented image back inside this frame's command buffer. The
    // present pass left the backbuffer in PresentSrc; return it there afterwards.
    RhiBuffer* screenshotBuffer = nullptr;
    auto screenshotPending = std::move(screenshotPath);
    screenshotPath.clear();
    // Texture dump: one mip level of a material texture read back after this frame.
    RhiBuffer* textureDumpBuffer = nullptr;
    uint32_t dumpWidth = 0;
    uint32_t dumpHeight = 0;
    auto dumpPending = std::move(textureDump);
    textureDump.reset();
    {
        PROFILE_ZONE("Record");
        PROFILE_ZONE_VALUE(frameGraph.passCount() - frameGraph.culledCount());
        cmd->reset();
        cmd->begin();
        frameGraph.execute(cmd);
        gpuScene.afterExecute(frameGraph, frame);
        recordTextureInspect(cmd);
        if (dumpPending.has_value()) {
            auto texIt = textureCache.find(dumpPending->material);
            if (texIt != textureCache.end() && dumpPending->level < texIt->second.mipLevels) {
                const auto& cached = texIt->second;
                dumpWidth = std::max(1u, cached.width >> dumpPending->level);
                dumpHeight = std::max(1u, cached.height >> dumpPending->level);
                RhiBufferDesc desc = {
                    .size = (uint64_t) dumpWidth * dumpHeight * 4,
                    .usage = RhiBufferUsage::TransferDst,
                    .memory = RhiMemoryUsage::CpuToGpu,
                };
                textureDumpBuffer = device->createBuffer(desc);
                std::array<RhiTextureBarrierDesc, 1> toTransfer = {{{.texture = cached.texture, .oldState = RhiTextureState::ShaderReadOnly, .newState = RhiTextureState::TransferSrc}}};
                cmd->pipelineBarrier(toTransfer);
                cmd->copyTextureToBuffer(cached.texture, textureDumpBuffer, {.width = dumpWidth, .height = dumpHeight, .mipLevel = dumpPending->level});
                std::array<RhiTextureBarrierDesc, 1> toShader = {{{.texture = cached.texture, .oldState = RhiTextureState::TransferSrc, .newState = RhiTextureState::ShaderReadOnly}}};
                cmd->pipelineBarrier(toShader);
            } else {
                std::println(stderr, "dump-texture: material {} level {} not found", dumpPending->material, dumpPending->level);
            }
        }
        if (!screenshotPending.empty()) {
            RhiBufferDesc desc = {
                .size = (uint64_t) ext.width * ext.height * 4,
                .usage = RhiBufferUsage::TransferDst,
                .memory = RhiMemoryUsage::CpuToGpu,
            };
            screenshotBuffer = device->createBuffer(desc);
            auto* backbuffer = swapchain->image(*index);
            std::array<RhiTextureBarrierDesc, 1> toTransfer = {{{.texture = backbuffer, .oldState = RhiTextureState::PresentSrc, .newState = RhiTextureState::TransferSrc}}};
            cmd->pipelineBarrier(toTransfer);
            cmd->copyTextureToBuffer(backbuffer, screenshotBuffer, {.width = ext.width, .height = ext.height});
            std::array<RhiTextureBarrierDesc, 1> toPresent = {{{.texture = backbuffer, .oldState = RhiTextureState::TransferSrc, .newState = RhiTextureState::PresentSrc}}};
            cmd->pipelineBarrier(toPresent);
        }
        cmd->end();
    }
    slotDrawLogs[currentFrame] = frameGraph.drawLog();
    if (frame % 60 == 0) {
        // Headless-checkable summary of what the frame drew and what the scene holds.
        const auto& stats = cmd->stats();
        OBS_EVENT("Render", "RenderStats", "frame")
            .field("frame", (int64_t) frame)
            .field("draws", (int64_t) stats.draws)
            .field("dispatches", (int64_t) stats.dispatches)
            .field("barriers", (int64_t) stats.barriers)
            .field("primitives", (int64_t) stats.primitives)
            .field("instances", (int64_t) gpuInstances.size())
            .field("culled", (int64_t) debugCulledInstances)
            .field("cascades", (int64_t) debugCascadeCount)
            .field("shadow_culled", (int64_t) debugShadowCulled)
            .field("meshes", (int64_t) gpuScene.meshRanges().size())
            .field("textures", (int64_t) textureCache.size())
            .field("logged_draws", (int64_t) slotDrawLogs[currentFrame].size())
            .field("instance_buffer_bytes", (int64_t) gpuScene.instanceBufferBytes())
            .field("instance_upload_bytes", (int64_t) gpuScene.lastInstanceUploadBytes())
            .field("geometry_pool_bytes", (int64_t) gpuScene.geometryPoolBytes())
            .field("material_count", (int64_t) gpuScene.materialCount());
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

    if (textureDumpBuffer != nullptr) {
        PROFILE_ZONE("TextureDump");
        device->waitForFence(inflightFences[currentFrame]);
        auto count = (size_t) dumpWidth * dumpHeight * 4;
        std::vector<uint8_t> rgba(count);
        auto* mapped = static_cast<const uint8_t*>(device->mapBuffer(textureDumpBuffer));
        memcpy(rgba.data(), mapped, count);
        device->unmapBuffer(textureDumpBuffer);
        device->destroyBuffer(textureDumpBuffer);
        bool ok = writeScreenshotPng(dumpPending->path.c_str(), rgba, dumpWidth, dumpHeight);
        std::println("{}: {} material {} level {} ({}x{})", ok ? "Texture dump written" : "Texture dump failed", dumpPending->path, dumpPending->material, dumpPending->level, dumpWidth, dumpHeight);
        OBS_EVENT("Render", "TextureDump", "texture")
            .field("material", (int64_t) dumpPending->material)
            .field("level", (int64_t) dumpPending->level)
            .field("path", dumpPending->path)
            .field("width", (int64_t) dumpWidth)
            .field("height", (int64_t) dumpHeight)
            .field("ok", ok);
    }
    if (screenshotBuffer != nullptr) {
        PROFILE_ZONE("Screenshot");
        device->waitForFence(inflightFences[currentFrame]);
        auto count = (size_t) ext.width * ext.height * 4;
        std::vector<uint8_t> rgba(count);
        auto* mapped = static_cast<const uint8_t*>(device->mapBuffer(screenshotBuffer));
        memcpy(rgba.data(), mapped, count);
        device->unmapBuffer(screenshotBuffer);
        device->destroyBuffer(screenshotBuffer);
        auto format = swapchain->colorFormat();
        if (format == RhiFormat::B8G8R8A8_SRGB || format == RhiFormat::B8G8R8A8_UNORM) {
            for (size_t i = 0; i < count; i += 4) {
                std::swap(rgba[i], rgba[i + 2]);
            }
        }
        for (size_t i = 3; i < count; i += 4) {
            rgba[i] = 255; // swapchain alpha is undefined for the viewer
        }
        bool ok = writeScreenshotPng(screenshotPending.c_str(), rgba, ext.width, ext.height);
        std::println("{}: {} ({}x{})", ok ? "Screenshot written" : "Screenshot failed", screenshotPending, ext.width, ext.height);
        OBS_EVENT("Render", "Screenshot", "frame")
            .field("frame", (int64_t) frame)
            .field("path", screenshotPending)
            .field("width", (int64_t) ext.width)
            .field("height", (int64_t) ext.height)
            .field("ok", ok);
    }
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
    if (texturePreview.imguiId != 0) {
        editorUI->unregisterTexture(texturePreview.imguiId);
    }
    if (texturePreview.texture != nullptr) {
        device->destroyTexture(texturePreview.texture);
    }
    texturePreview = {};

    editorUI->shutdown();
    editorUI = nullptr;

    if (geometryDescriptorPool) {
        device->freeDescriptorSets(geometryDescriptorPool, geometryDescriptorSets);
        device->destroyDescriptorPool(geometryDescriptorPool);
    }

    for (auto& [idx, cached] : textureCache) {
        device->destroyTexture(cached.texture);
    }
    textureCache.clear();

    gpuScene.destroy();

    shadowPass.destroy(device);
    depthPrepass.destroy(device);
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
    device->destroySampler(shadowSampler);
    device->destroySampler(materialSampler);
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
