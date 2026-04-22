#include "renderer.h"
#include "aapass.h"
#include "blitpass.h"
#include "material.h"
#include "mesh.h"
#include "presentpass.h"
#include "rendersnapshot.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "rhieditorui.h"
#include "rhieditoruivulkan.h"
#include "rhiswapchain.h"
#include "shadowpass.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstring>
#include <limits>

auto Renderer::init(RhiDevice* rhiDevice, SDL_Window* window) -> std::expected<void, int> {
    using enum RhiFormat;

    device = rhiDevice;

    swapchain = device->createSwapchain(window);
    if (swapchain == nullptr) {
        return std::unexpected(1);
    }

    resourcePool.init(device);
    frameGraph.setResourcePool(&resourcePool);

    auto imgCount = swapchain->imageCount();
    auto ext = swapchain->extent();
    auto colorFmt = swapchain->colorFormat();
    auto depthFmt = swapchain->depthFormat();

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
        .initialData = fallbackPixels.data(),
        .initialDataSize = 64 * 64 * 4,
    };
    fallbackTexture = device->createTexture(fallbackDesc);

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
    for (uint32_t i = 0; i < imgCount; i++) {
        imageAvailableSemaphores[i] = device->createSemaphore();
        renderFinishedSemaphores[i] = device->createSemaphore();
        inflightFences[i] = device->createFence(true);
    }

    // Editor UI
    editorUI = std::make_unique<RhiEditorUIVulkan>();
    editorUI->init({
        .window = window,
        .device = device,
        .colorFormat = swapchain->colorFormat(),
        .imageCount = swapchain->imageCount(),
    });

    fgPreviews.init(device, editorUI.get(), textureSampler);

    return {};
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
    return snap;
}

auto Renderer::uploadRenderWorld(const RenderWorld& world, const MeshLibrary& meshLib, const MaterialLibrary& matLib) -> void {
    using enum RhiDescriptorType;
    using enum RhiMemoryUsage;

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
            if (gpuInstances[m].mesh != world.meshInstances[m].mesh || gpuInstances[m].material != world.meshInstances[m].material) {
                geometryChanged = true;
                break;
            }
        }
    }

    gpuInstances.resize(world.meshInstances.size());
    for (size_t m = 0; m < world.meshInstances.size(); m++) {
        const auto& inst = world.meshInstances[m];
        gpuInstances[m] = {.mesh = inst.mesh, .material = inst.material, .transform = inst.worldTransform};
    }

    if (!geometryChanged) {
        return;
    }

    device->waitIdle();

    for (auto& [idx, cached] : meshCache) {
        device->destroyBuffer(cached.vertexBuffer);
        device->destroyBuffer(cached.indexBuffer);
    }
    meshCache.clear();
    for (auto& [idx, cached] : textureCache) {
        device->destroyTexture(cached.texture);
    }
    textureCache.clear();

    for (const auto& inst : world.meshInstances) {
        if (inst.mesh && !meshCache.contains(inst.mesh.index)) {
            const auto* meshData = meshLib.get(inst.mesh);
            if (meshData && !meshData->vertices.empty()) {
                CachedMesh cached;
                cached.indexCount = (uint32_t) meshData->indices.size();

                auto vbSize = (uint64_t) (meshData->vertices.size() * sizeof(Vertex));
                RhiBufferDesc stagingDesc = {.size = vbSize, .usage = RhiBufferUsage::TransferSrc, .memory = CpuToGpu};
                auto* vStaging = device->createBuffer(stagingDesc);
                auto* data = device->mapBuffer(vStaging);
                memcpy(data, meshData->vertices.data(), vbSize);
                device->unmapBuffer(vStaging);

                RhiBufferDesc vbDesc = {.size = vbSize, .usage = RhiBufferUsage::TransferDst | RhiBufferUsage::Vertex, .memory = GpuOnly};
                cached.vertexBuffer = device->createBuffer(vbDesc);
                device->copyBuffer(vStaging, cached.vertexBuffer, vbSize);
                device->destroyBuffer(vStaging);

                auto ibSize = (uint64_t) (meshData->indices.size() * sizeof(uint32_t));
                stagingDesc.size = ibSize;
                auto* iStaging = device->createBuffer(stagingDesc);
                data = device->mapBuffer(iStaging);
                memcpy(data, meshData->indices.data(), ibSize);
                device->unmapBuffer(iStaging);

                RhiBufferDesc ibDesc = {.size = ibSize, .usage = RhiBufferUsage::TransferDst | RhiBufferUsage::Index, .memory = GpuOnly};
                cached.indexBuffer = device->createBuffer(ibDesc);
                device->copyBuffer(iStaging, cached.indexBuffer, ibSize);
                device->destroyBuffer(iStaging);

                meshCache[inst.mesh.index] = cached;
            }
        }

        if (inst.material && !textureCache.contains(inst.material.index)) {
            const auto* matData = matLib.get(inst.material);
            if (matData && !matData->texPixels.empty()) {
                RhiTextureDesc texDesc = {
                    .width = (uint32_t) matData->texWidth,
                    .height = (uint32_t) matData->texHeight,
                    .format = RhiFormat::R8G8B8A8_SRGB,
                    .initialData = matData->texPixels.data(),
                    .initialDataSize = (uint64_t) (matData->texWidth * matData->texHeight * 4),
                };
                textureCache[inst.material.index] = {.texture = device->createTexture(texDesc)};
            }
        }
    }

    for (auto* ds : geometryDescriptorSets) {
        delete ds;
    }
    geometryDescriptorSets.clear();
    if (geometryDescriptorPool) {
        device->destroyDescriptorPool(geometryDescriptorPool);
        geometryDescriptorPool = nullptr;
    }

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
    geometryDescriptorSets = device->allocateDescriptorSets(geometryDescriptorPool, geometryPass.descriptorSetLayout(), totalSets);

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
    device->waitForFence(inflightFences[currentFrame]);

    auto index = swapchain->acquireNextImage(imageAvailableSemaphores[currentFrame]);
    if (!index) {
        if (index.error() == 2) {
            swapchain->recreate();
            resourcePool.flush();
            currentFrame = 0;
        }
        return;
    }

    device->resetFence(inflightFences[currentFrame]);

    UniformBufferObject ubo = {
        .view = snapshot.viewMatrix,
        .proj = snapshot.projMatrix,
    };
    memcpy(uniformBuffersMapped[*index], &ubo, sizeof(ubo));

    // Build frame graph
    auto ext = swapchain->extent();
    frameGraph.reset();

    auto colorHandle = frameGraph.importTexture("backbuffer", swapchain->image(*index), {ext.width, ext.height, swapchain->colorFormat()});
    auto depthHandle = frameGraph.importTexture("depth", swapchain->depthImage(), {ext.width, ext.height, swapchain->depthFormat()});

    auto imageIdx = *index;
    auto instanceCount = (uint32_t) gpuInstances.size();

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
    auto shadowUp = std::abs(glm::dot(lighting.direction, snapshot.worldUp)) > 0.99f
                        ? glm::normalize(glm::cross(lighting.direction, glm::vec3(1.0f, 0.0f, 0.0f)))
                        : snapshot.worldUp;
    auto lightView = glm::lookAt(lightPos, sceneCenter, shadowUp);
    auto lightProj = glm::ortho(-shadowHalf, shadowHalf, -shadowHalf, shadowHalf, 0.1f, 2.0f * lightDistance);
    auto lightViewProj = lightProj * lightView;

    auto invViewProj = glm::inverse(snapshot.projMatrix * snapshot.viewMatrix);

    const auto& shadowData = shadowPass.addPass(frameGraph, shadowExtent, swapchain->depthFormat(), lightViewProj, gpuInstances, meshCache);

    const auto& geomData = geometryPass.addPass(frameGraph, depthHandle, ext, imageIdx, instanceCount, gpuInstances, meshCache, geometryDescriptorSets);

    const auto& lightData = lightingPass.addPass(frameGraph,
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

    // AA sits between the scene render and the overlays, so debug lines, gizmos and UI
    // are drawn on top of the anti-aliased image instead of being smeared by it.
    const auto& aaData = addAAPass(frameGraph, lightData.sceneColor, ext, swapchain->colorFormat());
    auto sceneColor = aaData.sceneColorAA;

    debugRenderer.addPass(frameGraph, sceneColor, depthHandle, ext, snapshot.debugData, imageIdx);

    auto gizmoRequests = gizmoUpdate(snapshot, ext);
    gizmoPass.addPass(frameGraph, sceneColor, ext, gizmoRequests, imageIdx);

    editorUIPass.addPass(frameGraph, sceneColor, ext, editorUI.get(), snapshot.imguiSnapshot);

    addBlitPass(frameGraph, "BlitToBackbuffer", sceneColor, colorHandle, ext, ext);
    addPresentPass(frameGraph, colorHandle);

    frameGraph.compile();

    auto* cmd = cmdBuffers[*index];
    cmd->reset();
    cmd->begin();

    frameGraph.execute(cmd);

    cmd->end();

    RhiSubmitInfo submitInfo = {
        .waitSemaphore = imageAvailableSemaphores[currentFrame],
        .signalSemaphore = renderFinishedSemaphores[currentFrame],
        .fence = inflightFences[currentFrame],
    };
    device->submitCommandBuffer(cmd, submitInfo);
    if (!device->present(swapchain, renderFinishedSemaphores[currentFrame], *index)) {
        swapchain->recreate();
        resourcePool.flush();
        currentFrame = 0;
        return;
    }

    currentFrame = (currentFrame + 1) % swapchain->imageCount();
}

auto Renderer::destroy() -> void {
    device->waitIdle();

    fgPreviews.shutdown();

    editorUI->shutdown();
    editorUI.reset();

    for (auto* ds : geometryDescriptorSets) {
        delete ds;
    }
    if (geometryDescriptorPool) {
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
    debugRenderer.destroy(device);
    gizmoPass.destroy(device);

    for (uint32_t i = 0; i < swapchain->imageCount(); i++) {
        device->unmapBuffer(uniformBuffers[i]);
        device->destroyBuffer(uniformBuffers[i]);
    }

    device->destroySampler(textureSampler);
    device->destroyTexture(fallbackTexture);

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
