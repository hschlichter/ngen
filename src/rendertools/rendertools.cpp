#include "rendertools.h"

#include "deletionqueue.h"
#include "framegraph.h"
#include "gpuscene.h"
#include "packedtexture.h"
#include "pipelineregistry.h"
#include "previewtextures.h"
#include "profile.h"
#include "profileconfig.h"
#include "renderer.h"
#include "rendersnapshot.h"
#include "resourcepool.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "trace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>
#include <utility>

namespace {

// Preview extent for the inspector: the level scaled to fit a 256 box, up or down, so a 4x4 level shows as blocks instead of a dot.
auto inspectPreviewExtent(uint32_t w, uint32_t h) -> RhiExtent2D {
    constexpr uint32_t box = 256;
    auto maxSide = std::max(w, h);
    if (maxSide == 0) {
        return {box, box};
    }
    return {std::max(1u, (w * box) / maxSide), std::max(1u, (h * box) / maxSide)};
}

} // namespace

auto RenderTools::startFrame() -> void {
    {
        std::lock_guard lock(resultsMutex);
        taken = std::exchange(published, {});
    }
    next = {};
}

auto RenderTools::requestScreenshot(std::string path) -> void {
    pendingScreenshots.push_back(std::move(path));
}

auto RenderTools::requestTextureDump(uint32_t texture, uint32_t level, std::string path) -> void {
    pendingDumps.push_back({.texture = texture, .level = level, .path = std::move(path)});
}

auto RenderTools::fillSnapshot(RenderSnapshot& snapshot) -> void {
    auto frame = std::make_unique<RenderToolsFrame>(std::move(next));
    frame->textureInspect = textureInspect;
    frame->textureDumps = std::exchange(pendingDumps, {});
    frame->screenshots = std::exchange(pendingScreenshots, {});
    frame->frameImages = profileconfig::tracyFrameImages.get();
    frame->profileFrame = profile::frameMarks();
    snapshot.setExtension(index(), std::move(frame));
    next = {};
}

auto RenderTools::init(const RenderExtensionContext& extensionContext) -> bool {
    context.emplace(extensionContext);
    previews.init(&context->device, context->previews, context->textureSampler, &context->deletionQueue);
    frameImages.init(&context->device, context->frameSlots);
    return true;
}

auto RenderTools::afterFence(Renderer& renderer, uint32_t slot) -> void {
    frameImages.afterFence(slot);
    // GPU counters: the zones of the frame read back, and while on the pipeline statistics its passes gathered.
    auto state = renderer.state();
    if (!frameRequest.counters || state.gpuFrame == 0 || state.gpuFrame == countersFrame || state.gpuZones.empty()) {
        return;
    }
    countersFrame = state.gpuFrame;
    uint64_t minStart = UINT64_MAX;
    for (const auto& zone : state.gpuZones) {
        minStart = std::min(minStart, zone.startNs);
    }
    GpuCounters counters = {
        .frame = state.gpuFrame,
        .gpuFrameMs = state.gpuFrameMs,
        .width = state.swapchainExtent.width,
        .height = state.swapchainExtent.height,
    };
    counters.zones.reserve(state.gpuZones.size());
    for (const auto& zone : state.gpuZones) {
        counters.zones.push_back({.name = zone.name, .depth = zone.depth, .startMs = (double) (zone.startNs - minStart) * 1e-6, .ms = (double) (zone.endNs - zone.startNs) * 1e-6});
    }
    if (context->device.collectPipelineStats(renderer.commandBuffer(slot), pipelineStatsScratch)) {
        for (const auto& zone : pipelineStatsScratch) {
            counters.passes.push_back({.name = zone.name, .stats = zone.stats});
        }
    }
    std::lock_guard lock(resultsMutex);
    published.counters.push_back(std::move(counters));
    // Bounded: a main thread that stops taking keeps only recent frames.
    if (published.counters.size() > 300) {
        published.counters.erase(published.counters.begin(), published.counters.end() - 300);
    }
}

auto RenderTools::beginFrame(Renderer& renderer, RenderSnapshot& snapshot, RenderFrame& frame) -> void {
    if (auto* request = snapshot.extension<RenderToolsFrame>(index())) {
        frameRequest = std::move(*request);
    } else {
        frameRequest = {};
    }
    for (auto& dump : frameRequest.textureDumps) {
        dumps.push_back(std::move(dump));
    }
    for (auto& path : frameRequest.screenshots) {
        screenshots.push_back(std::move(path));
    }
    frameDebugPending = frameDebugPending || frameRequest.frameDebug;
    // A screenshot frame leaves the UI out, unless asked to keep it.
    frame.hideUi = !screenshots.empty() && !screenshotsShowUi;
    previews.setFrame(frame.frame);
    auto& frameGraph = context->frameGraph;
    if (frameRequest.frameGraphDebug != frameGraphDebugOn) {
        frameGraphDebugOn = frameRequest.frameGraphDebug;
        if (frameGraphDebugOn) {
            frameGraph.setDebugCaptureHook([this](RhiCommandBuffer* cmd, const FgCapturedResource& view) { previews.capture(cmd, view); });
        } else {
            frameGraph.setDebugCaptureHook(nullptr);
        }
    }
    // The render debug snapshot lists the frame's draws: the draw log and the culling's commands read back.
    renderer.setDrawLogEnabled(frameRequest.renderDebug);
}

auto RenderTools::beforeExecute(Renderer& /*renderer*/, RhiCommandBuffer* cmd, const RenderFrame& /*frame*/) -> void {
    cmd->setCommandLog(frameDebugPending);
    context->frameGraph.setCommandLogEnabled(frameDebugPending);
    context->frameGraph.setPipelineStatsEnabled(frameRequest.counters && context->device.limits().pipelineStatistics);
}

auto RenderTools::afterExecute(Renderer& renderer, RhiCommandBuffer* cmd, const RenderFrame& frame) -> void {
    if (std::exchange(frameDebugPending, false)) {
        frameDebugCapture = buildFrameDebugCapture(buildFrameGraphDebugSnapshot(renderer), context->device, frame.frame);
        cmd->setCommandLog(false);
        context->frameGraph.setCommandLogEnabled(false);
    }
    recordTextureInspect(renderer, cmd);
    recordTextureDump(renderer, cmd);
    recordScreenshot(cmd, frame);
    if (frameRequest.frameImages && profile::tracyConnected()) {
        frameImages.record(cmd, frame, renderer.state().swapchainFormat, frameRequest.profileFrame);
    }
}

auto RenderTools::afterSubmit(Renderer& renderer, const RenderFrame& frame) -> void {
    if (readbacks.empty()) {
        return;
    }
    PROFILE_ZONE("ReadbackFiles", Render);
    renderer.waitForFrame();
    auto format = renderer.state().swapchainFormat;
    for (auto& readback : readbacks) {
        auto count = (size_t) readback.width * readback.height * 4;
        std::vector<uint8_t> rgba(count);
        auto* mapped = static_cast<const uint8_t*>(context->device.mapBuffer(readback.buffer));
        std::memcpy(rgba.data(), mapped, count);
        context->device.unmapBuffer(readback.buffer);
        context->device.destroyBuffer(readback.buffer);
        if (readback.hdr) {
            // An HDR texture's PNG: its linear radiance clamped to 1 and encoded as sRGB.
            for (size_t i = 0; i < count; i += 4) {
                uint32_t texel = 0;
                std::memcpy(&texel, &rgba[i], 4);
                auto rgb = decodeRgb9e5(texel);
                for (size_t c = 0; c < 3; c++) {
                    float v = std::clamp(rgb[c], 0.0f, 1.0f);
                    float encoded = v <= 0.0031308f ? v * 12.92f : (1.055f * std::pow(v, 1.0f / 2.4f)) - 0.055f;
                    rgba[i + c] = (uint8_t) std::lround(encoded * 255.0f);
                }
                rgba[i + 3] = 255;
            }
        }
        if (!readback.screenshot) {
            bool ok = writeScreenshotPng(readback.path.c_str(), rgba, readback.width, readback.height);
            if (ok) {
                TRACE_EVENT("Render", "TextureDump", readback.path)
                    .text(std::format("texture dump written: {} texture {} level {} ({}x{})", readback.path, readback.texture, readback.level, readback.width, readback.height))
                    .field("texture", (int64_t) readback.texture)
                    .field("level", (int64_t) readback.level);
            } else {
                TRACE_ERROR("Render", "TextureDumpFailed", readback.path).text(std::format("cannot write texture dump {}", readback.path));
            }
            continue;
        }
        if (format == RhiFormat::B8G8R8A8_SRGB || format == RhiFormat::B8G8R8A8_UNORM) {
            for (size_t i = 0; i < count; i += 4) {
                std::swap(rgba[i], rgba[i + 2]);
            }
        }
        for (size_t i = 3; i < count; i += 4) {
            rgba[i] = 255; // swapchain alpha is undefined for the viewer
        }
        bool ok = writeScreenshotPng(readback.path.c_str(), rgba, readback.width, readback.height);
        {
            std::lock_guard lock(resultsMutex);
            published.screenshots.push_back({.path = readback.path, .ok = ok, .frame = frame.frame, .width = readback.width, .height = readback.height});
        }
        if (ok) {
            TRACE_EVENT("Render", "Screenshot", readback.path)
                .text(std::format("screenshot written: {} ({}x{})", readback.path, readback.width, readback.height))
                .field("frame", (int64_t) frame.frame);
        } else {
            TRACE_ERROR("Render", "ScreenshotFailed", readback.path).text(std::format("cannot write screenshot {}", readback.path)).field("frame", (int64_t) frame.frame);
        }
    }
    readbacks.clear();
}

auto RenderTools::endFrame(Renderer& renderer) -> void {
    std::optional<FrameGraphDebugSnapshot> frameGraph;
    if (frameRequest.frameGraphDebug) {
        frameGraph = buildFrameGraphDebugSnapshot(renderer);
        frameGraph->frameIndex = ++frameGraphDebugFrames;
    }
    std::optional<RenderDebugSnapshot> renderDebug;
    if (frameRequest.renderDebug) {
        renderDebug = buildRenderDebugSnapshot(renderer);
    }
    if (!frameGraph && !renderDebug && !frameDebugCapture) {
        return;
    }
    std::lock_guard lock(resultsMutex);
    if (frameGraph) {
        published.frameGraph = std::move(frameGraph);
    }
    if (renderDebug) {
        published.renderDebug = std::move(renderDebug);
    }
    if (frameDebugCapture) {
        published.frameDebug = std::exchange(frameDebugCapture, std::nullopt);
    }
}

auto RenderTools::destroy() -> void {
    previews.shutdown();
    frameImages.destroy();
    if (texturePreview.id != 0 && context->previews != nullptr) {
        context->previews->unregisterTexture(texturePreview.id);
    }
    if (texturePreview.texture != nullptr) {
        context->device.destroyTexture(texturePreview.texture);
    }
    texturePreview = {};
    for (auto& readback : readbacks) {
        context->device.destroyBuffer(readback.buffer);
    }
    readbacks.clear();
}

auto RenderTools::buildFrameGraphDebugSnapshot(const Renderer& renderer) const -> FrameGraphDebugSnapshot {
    auto snap = renderer.frameGraphRef().buildDebugSnapshot();
    previews.annotate(snap);
    auto state = renderer.state();
    snap.gpuFrameMs = state.gpuFrameMs;
    for (auto& pass : snap.passes) {
        for (const auto& timing : state.passGpuTimes) {
            if (pass.name == timing.name) {
                pass.gpuTimeMs = timing.ms;
                break;
            }
        }
    }
    return snap;
}

auto RenderTools::buildRenderDebugSnapshot(const Renderer& renderer) const -> RenderDebugSnapshot {
    auto state = renderer.state();
    const auto& gpuScene = *state.gpuScene;
    RenderDebugSnapshot snap;
    snap.frameIndex = state.frame;
    snap.pipelines = renderer.pipelineInfos();
    snap.limits = state.device->limits();
    snap.validation = validationEnabled;
    snap.swapchainExtent = state.swapchainExtent;
    snap.swapchainFormat = state.swapchainFormat;
    snap.swapchainImages = state.swapchainImages;
    snap.currentSlot = state.slot;
    state.device->allocations(snap.allocations);
    state.device->memoryHeaps(snap.heaps);
    snap.geometryPool = renderer.sceneGpuStats().geometryPool;

    snap.culledInstances = state.culledInstances;
    std::unordered_map<uint32_t, uint32_t> instancesPerMesh;
    uint32_t live = 0;
    for (const auto& inst : state.instances) {
        if (!inst.alive) {
            continue;
        }
        live++;
        instancesPerMesh[inst.mesh.index]++;
        if (inst.cutout) {
            snap.cutoutInstances++;
        }
    }
    snap.instanceCount = live;
    snap.cutoutMaterials = gpuScene.cutoutMaterialCount();
    snap.meshes.reserve(gpuScene.meshRanges().size());
    for (const auto& [index, cached] : gpuScene.meshRanges()) {
        snap.meshes.push_back({
            .meshIndex = index,
            .vertexCount = cached.vertexCount,
            .triangleCount = cached.meshletTriangleCount,
            .meshletCount = cached.meshletCount,
            .vertexBytes = cached.vertexBytes,
            .meshletBytes = cached.meshletBytes,
            .instances = instancesPerMesh[index],
        });
    }
    snap.materialCount = gpuScene.materialCount();
    snap.materialsWithTexture = gpuScene.texturedMaterialCount();
    snap.textures.reserve(gpuScene.textures().size());
    for (const auto& [index, info] : gpuScene.textures()) {
        snap.textures.push_back({
            .textureIndex = index,
            .slot = info.slot,
            .materials = gpuScene.materialsUsingTexture(index),
            .width = info.width,
            .height = info.height,
            .mipLevels = info.mipLevels,
            .format = info.format,
            .bytes = info.bytes,
        });
    }
    std::ranges::sort(snap.textures, [](const RenderDebugTexture& a, const RenderDebugTexture& b) { return a.textureIndex < b.textureIndex; });
    snap.inspect = textureInspectResult;
    snap.lightCount = state.lightCount;
    snap.hasSun = state.hasSun;
    snap.sunDirection = state.sun.direction;
    snap.sunRadiance = state.sun.radiance;
    snap.sunShadowColor = state.sun.shadowColor;
    snap.shadowMapExtent = state.shadowAtlasExtent;
    if (state.environment != nullptr) {
        // The constant term's basis value over π: E00 · 0.282095 / π is the image's mean radiance.
        snap.hasSky = true;
        snap.skyDrawn = state.skyDrawn;
        snap.skyMeanRadiance = state.environment->irradiance[0] * (0.282095f / glm::pi<float>()) * state.environment->radiance;
    }

    auto fg = buildFrameGraphDebugSnapshot(renderer);
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
    state.resourcePool->forEach([&](const ResourcePoolKey& key, bool inUse) {
        snap.poolTextures.push_back({.width = key.width, .height = key.height, .format = key.format, .inUse = inUse});
    });
    snap.poolAllocationsTotal = state.resourcePool->stats().allocationsTotal;
    snap.draws.assign(state.drawLog.begin(), state.drawLog.end());
    return snap;
}

auto RenderTools::releaseTexturePreview(uint64_t frame) -> void {
    if (texturePreview.id != 0) {
        context->deletionQueue.defer(frame, [previews = context->previews, id = texturePreview.id] { previews->unregisterTexture(id); });
    }
    if (texturePreview.texture != nullptr) {
        context->deletionQueue.deferTexture(frame, texturePreview.texture);
    }
    texturePreview = {};
}

auto RenderTools::recordTextureInspect(const Renderer& renderer, RhiCommandBuffer* cmd) -> void {
    textureInspectResult = {};
    const auto& request = frameRequest.textureInspect;
    if (!request.enabled || context->previews == nullptr) {
        if (texturePreview.texture != nullptr) {
            releaseTexturePreview(renderer.state().frame);
        }
        return;
    }
    const auto& inspected = renderer.state().gpuScene->textures();
    auto texIt = inspected.find(request.texture);
    if (texIt == inspected.end() || texIt->second.slot == GpuScene::fallbackSlot) {
        return;
    }
    const auto& cached = texIt->second;
    auto level = std::min(request.level, cached.mipLevels - 1);
    uint32_t levelW = std::max(1u, cached.width >> level);
    uint32_t levelH = std::max(1u, cached.height >> level);
    auto ext = inspectPreviewExtent(levelW, levelH);

    if (texturePreview.texture == nullptr || texturePreview.width != ext.width || texturePreview.height != ext.height) {
        releaseTexturePreview(renderer.state().frame);
        // An HDR texture (the sky) is blitted into a half-float preview: its own format can't be a blit's destination.
        RhiTextureDesc desc = {
            .width = ext.width,
            .height = ext.height,
            .format = cached.format == RhiFormat::E5B9G9R9_UFLOAT ? RhiFormat::R16G16B16A16_SFLOAT : cached.format,
            .usage = RhiTextureUsage::Sampled | RhiTextureUsage::TransferDst,
            .debugName = "inspector.preview",
        };
        texturePreview.texture = context->device.createTexture(desc);
        texturePreview.width = ext.width;
        texturePreview.height = ext.height;
        if (texturePreview.texture != nullptr) {
            texturePreview.id = context->previews->registerTexture(texturePreview.texture, context->textureSampler);
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
        .texture = request.texture,
        .level = level,
        .mipLevels = cached.mipLevels,
        .levelWidth = levelW,
        .levelHeight = levelH,
        .levelBytes = (uint64_t) levelW * levelH * 4,
        .previewTextureId = texturePreview.id,
        .previewWidth = ext.width,
        .previewHeight = ext.height,
    };
}

auto RenderTools::recordTextureDump(const Renderer& renderer, RhiCommandBuffer* cmd) -> void {
    const auto& textures = renderer.state().gpuScene->textures();
    for (auto& dump : std::exchange(dumps, {})) {
        auto texIt = textures.find(dump.texture);
        if (texIt == textures.end() || texIt->second.slot == GpuScene::fallbackSlot || dump.level >= texIt->second.mipLevels) {
            TRACE_ERROR("Render", "TextureDumpFailed", dump.path).text(std::format("dump-texture: texture {} level {} not found", dump.texture, dump.level));
            continue;
        }
        const auto& cached = texIt->second;
        auto width = std::max(1u, cached.width >> dump.level);
        auto height = std::max(1u, cached.height >> dump.level);
        RhiBufferDesc desc = {
            .size = (uint64_t) width * height * 4,
            .usage = RhiBufferUsage::TransferDst,
            .memory = RhiMemoryUsage::CpuToGpu,
            .debugName = "readback.texturedump",
        };
        auto* buffer = context->device.createBuffer(desc);
        std::array<RhiTextureBarrierDesc, 1> toTransfer = {{{.texture = cached.texture, .oldState = RhiTextureState::ShaderReadOnly, .newState = RhiTextureState::TransferSrc}}};
        cmd->pipelineBarrier(toTransfer);
        cmd->copyTextureToBuffer(cached.texture, buffer, {.width = width, .height = height, .mipLevel = dump.level});
        std::array<RhiTextureBarrierDesc, 1> toShader = {{{.texture = cached.texture, .oldState = RhiTextureState::TransferSrc, .newState = RhiTextureState::ShaderReadOnly}}};
        cmd->pipelineBarrier(toShader);
        readbacks.push_back({.buffer = buffer, .width = width, .height = height, .path = std::move(dump.path), .texture = dump.texture, .level = dump.level, .hdr = cached.format == RhiFormat::E5B9G9R9_UFLOAT});
    }
}

auto RenderTools::recordScreenshot(RhiCommandBuffer* cmd, const RenderFrame& frame) -> void {
    // Read the presented image back inside this frame's command buffer: the present pass left the backbuffer in PresentSrc, and it goes
    // back there afterwards.
    for (auto& path : std::exchange(screenshots, {})) {
        RhiBufferDesc desc = {
            .size = (uint64_t) frame.extent.width * frame.extent.height * 4,
            .usage = RhiBufferUsage::TransferDst,
            .memory = RhiMemoryUsage::CpuToGpu,
            .debugName = "readback.screenshot",
        };
        auto* buffer = context->device.createBuffer(desc);
        std::array<RhiTextureBarrierDesc, 1> toTransfer = {{{.texture = frame.backbuffer, .oldState = RhiTextureState::PresentSrc, .newState = RhiTextureState::TransferSrc}}};
        cmd->pipelineBarrier(toTransfer);
        cmd->copyTextureToBuffer(frame.backbuffer, buffer, {.width = frame.extent.width, .height = frame.extent.height});
        std::array<RhiTextureBarrierDesc, 1> toPresent = {{{.texture = frame.backbuffer, .oldState = RhiTextureState::TransferSrc, .newState = RhiTextureState::PresentSrc}}};
        cmd->pipelineBarrier(toPresent);
        readbacks.push_back({.buffer = buffer, .width = frame.extent.width, .height = frame.extent.height, .path = std::move(path), .screenshot = true});
    }
}
