#include "renderer.h"
#include "camera.h"
#include "debugdraw.h"
#include "material.h"
#include "mesh.h"
#include "rhicommandbuffer.h"
#include "rhidebugui.h"
#include "rhidebuguivulkan.h"
#include "rhidevice.h"
#include "rhiswapchain.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>

auto Renderer::init(RhiDevice* rhiDevice, SDL_Window* window) -> std::expected<void, int> {
    using enum RhiDescriptorType;
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

    // Forward pass pipeline
    vertShader = device->createShaderModule("shaders/triangle.vert.spv");
    fragShader = device->createShaderModule("shaders/triangle.frag.spv");

    std::array<RhiDescriptorBinding, 2> bindings = {{
        {.binding = 0, .type = UniformBuffer, .stage = RhiShaderStage::Vertex},
        {.binding = 1, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},
    }};
    descriptorSetLayout = device->createDescriptorSetLayout(bindings);

    std::array<RhiVertexAttribute, 4> vertexAttrs = {{
        {.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, position)},
        {.location = 1, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, normal)},
        {.location = 2, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, color)},
        {.location = 3, .binding = 0, .format = R32G32_SFLOAT, .offset = offsetof(struct Vertex, texCoord)},
    }};

    RhiGraphicsPipelineDesc pipelineDesc = {
        .vertexShader = vertShader,
        .fragmentShader = fragShader,
        .descriptorSetLayout = descriptorSetLayout,
        .colorFormats = {&colorFmt, 1},
        .depthFormat = depthFmt,
        .vertexStride = sizeof(Vertex),
        .vertexAttributes = vertexAttrs,
        .pushConstant = {.stage = RhiShaderStage::Vertex, .offset = 0, .size = sizeof(glm::mat4)},
        .viewportExtent = ext,
    };
    pipeline = device->createGraphicsPipeline(pipelineDesc);
    if (pipeline == nullptr) {
        return std::unexpected(1);
    }

    // Debug line pipeline
    debugVertShader = device->createShaderModule("shaders/debug.vert.spv");
    debugFragShader = device->createShaderModule("shaders/debug.frag.spv");

    std::array<RhiDescriptorBinding, 1> debugBindings = {{
        {.binding = 0, .type = UniformBuffer, .stage = RhiShaderStage::Vertex},
    }};
    debugDescriptorSetLayout = device->createDescriptorSetLayout(debugBindings);

    std::array<RhiVertexAttribute, 2> debugVertexAttrs = {{
        {.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = 0},
        {.location = 1, .binding = 0, .format = R32G32B32A32_SFLOAT, .offset = sizeof(float) * 3},
    }};

    RhiGraphicsPipelineDesc debugPipelineDesc = {
        .vertexShader = debugVertShader,
        .fragmentShader = debugFragShader,
        .descriptorSetLayout = debugDescriptorSetLayout,
        .colorFormats = {&colorFmt, 1},
        .depthFormat = depthFmt,
        .vertexStride = sizeof(DebugVertex),
        .vertexAttributes = debugVertexAttrs,
        .pushConstant = {},
        .viewportExtent = ext,
        .topology = RhiPrimitiveTopology::LineList,
        .depthTestEnable = true,
        .depthWriteEnable = false,
    };
    debugLinePipeline = device->createGraphicsPipeline(debugPipelineDesc);

    debugVertexBuffers.resize(imgCount);
    debugVertexBuffersMapped.resize(imgCount);
    for (uint32_t i = 0; i < imgCount; i++) {
        RhiBufferDesc vbDesc = {
            .size = debugMaxVertices * sizeof(DebugVertex),
            .usage = RhiBufferUsage::Vertex,
            .memory = RhiMemoryUsage::CpuToGpu,
        };
        debugVertexBuffers[i] = device->createBuffer(vbDesc);
        debugVertexBuffersMapped[i] = device->mapBuffer(debugVertexBuffers[i]);
    }

    debugDescriptorPool = device->createDescriptorPool(imgCount, debugBindings);
    debugDescriptorSets = device->allocateDescriptorSets(debugDescriptorPool, debugDescriptorSetLayout, imgCount);
    for (uint32_t i = 0; i < imgCount; i++) {
        std::array<RhiDescriptorWrite, 1> writes = {{
            {
                .binding = 0,
                .type = UniformBuffer,
                .buffer = uniformBuffers[i],
                .bufferRange = sizeof(UniformBufferObject),
            },
        }};
        device->updateDescriptorSet(debugDescriptorSets[i], writes);
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

    // Debug UI
    debugUI = std::make_unique<RhiDebugUIVulkan>();
    debugUI->init({
        .window = window,
        .device = device,
        .colorFormat = swapchain->colorFormat(),
        .imageCount = swapchain->imageCount(),
    });

    return {};
}

auto Renderer::uploadRenderWorld(const RenderWorld& world, const MeshLibrary& meshLib, const MaterialLibrary& matLib) -> void {
    using enum RhiDescriptorType;
    using enum RhiMemoryUsage;

    // Check if anything actually changed by comparing instance data
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

    // Check if we can do a transform-only update (no mesh/material changes, same count)
    bool geometryChanged = (gpuInstances.size() != world.meshInstances.size());
    if (!geometryChanged) {
        for (size_t m = 0; m < world.meshInstances.size(); m++) {
            if (gpuInstances[m].mesh != world.meshInstances[m].mesh || gpuInstances[m].material != world.meshInstances[m].material) {
                geometryChanged = true;
                break;
            }
        }
    }

    // Update instance list
    gpuInstances.resize(world.meshInstances.size());
    for (size_t m = 0; m < world.meshInstances.size(); m++) {
        const auto& inst = world.meshInstances[m];
        gpuInstances[m] = {.mesh = inst.mesh, .material = inst.material, .transform = inst.worldTransform};
    }

    // Transform-only update: no GPU resources need changing
    if (!geometryChanged) {
        return;
    }

    // Full geometry update: need to rebuild GPU resources
    device->waitIdle();

    // Destroy old caches
    for (auto& [idx, cached] : meshCache) {
        device->destroyBuffer(cached.vertexBuffer);
        device->destroyBuffer(cached.indexBuffer);
    }
    meshCache.clear();
    for (auto& [idx, cached] : textureCache) {
        device->destroyTexture(cached.texture);
    }
    textureCache.clear();

    // Upload unique meshes and textures
    for (const auto& inst : world.meshInstances) {
        // Mesh
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

        // Texture
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

    // Rebuild descriptor sets
    for (auto* ds : descriptorSets) {
        delete ds;
    }
    descriptorSets.clear();
    if (descriptorPool) {
        device->destroyDescriptorPool(descriptorPool);
        descriptorPool = nullptr;
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
    descriptorPool = device->createDescriptorPool(totalSets, poolBindings);
    descriptorSets = device->allocateDescriptorSets(descriptorPool, descriptorSetLayout, totalSets);

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
            device->updateDescriptorSet(descriptorSets[(i * instanceCount) + m], writes);
        }
    }
}

struct ForwardPassData {
    FgTextureHandle color;
    FgTextureHandle depth;
};

auto Renderer::render(const Camera& camera, SDL_Window* window, const DebugDrawData& debugData) -> void {
    device->waitForFence(inflightFences[currentFrame]);

    auto index = swapchain->acquireNextImage(imageAvailableSemaphores[currentFrame]);
    if (!index) {
        return;
    }

    device->resetFence(inflightFences[currentFrame]);

    int winW = 0;
    int winH = 0;
    SDL_GetWindowSizeInPixels(window, &winW, &winH);
    auto aspect = (float) winW / (float) winH;
    auto proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 3000.0f);
    proj[1][1] *= -1.0f;
    UniformBufferObject ubo = {
        .view = camera.viewMatrix(),
        .proj = proj,
    };
    memcpy(uniformBuffersMapped[*index], &ubo, sizeof(ubo));

    // Build frame graph
    auto ext = swapchain->extent();
    frameGraph.reset();

    auto colorHandle = frameGraph.importTexture(swapchain->image(*index), {ext.width, ext.height, swapchain->colorFormat()});
    auto depthHandle = frameGraph.importTexture(swapchain->depthImage(), {ext.width, ext.height, swapchain->depthFormat()});

    auto imageIdx = *index;
    auto instanceCount = (uint32_t) gpuInstances.size();

    frameGraph.addPass<ForwardPassData>(
        "ForwardPass",
        [&](FrameGraphBuilder& builder, ForwardPassData& data) {
            data.color = builder.write(colorHandle, FgAccessFlags::ColorAttachment);
            data.depth = builder.write(depthHandle, FgAccessFlags::DepthAttachment);
            builder.setSideEffects(true);
        },
        [this, imageIdx, instanceCount, ext](FrameGraphContext& ctx, const ForwardPassData& data) {
            auto* cmd = ctx.cmd();

            RhiRenderingAttachmentInfo colorAtt = {
                .texture = ctx.texture(data.color),
                .layout = RhiImageLayout::ColorAttachment,
                .clear = true,
                .clearColor = {0.1f, 0.1f, 0.1f, 1.0f},
            };
            RhiRenderingAttachmentInfo depthAtt = {
                .texture = ctx.texture(data.depth),
                .layout = RhiImageLayout::DepthStencilAttachment,
                .clear = true,
                .clearDepth = 1.0f,
            };
            RhiRenderingInfo renderInfo = {
                .extent = ext,
                .colorAttachments = {&colorAtt, 1},
                .depthAttachment = &depthAtt,
            };
            cmd->beginRendering(renderInfo);
            cmd->bindPipeline(pipeline);

            for (uint32_t m = 0; m < instanceCount; m++) {
                auto& inst = gpuInstances[m];
                auto meshIt = meshCache.find(inst.mesh.index);
                if (meshIt == meshCache.end()) {
                    continue;
                }
                auto& cached = meshIt->second;

                auto model = inst.transform;
                cmd->pushConstants(pipeline, RhiShaderStage::Vertex, 0, sizeof(glm::mat4), &model);
                cmd->bindVertexBuffer(cached.vertexBuffer);
                cmd->bindIndexBuffer(cached.indexBuffer);
                cmd->bindDescriptorSet(pipeline, descriptorSets[(imageIdx * instanceCount) + m]);
                cmd->drawIndexed(cached.indexCount, 1, 0, 0, 0);
            }

            cmd->endRendering();
        });

    debugRenderer.addPass(frameGraph,
                          colorHandle,
                          depthHandle,
                          ext,
                          debugData,
                          {
                              .pipeline = debugLinePipeline,
                              .vertexBuffer = debugVertexBuffers[imageIdx],
                              .descriptorSet = debugDescriptorSets[imageIdx],
                              .vertexBufferMapped = debugVertexBuffersMapped[imageIdx],
                              .maxVertices = debugMaxVertices,
                          });

    struct DebugUIPassData {
        FgTextureHandle color;
    };

    frameGraph.addPass<DebugUIPassData>(
        "DebugUIPass",
        [&](FrameGraphBuilder& builder, DebugUIPassData& data) {
            data.color = builder.write(colorHandle, FgAccessFlags::ColorAttachment);
            builder.setSideEffects(true);
        },
        [this, ext](FrameGraphContext& ctx, const DebugUIPassData& data) {
            auto* cmd = ctx.cmd();

            RhiRenderingAttachmentInfo colorAtt = {
                .texture = ctx.texture(data.color),
                .layout = RhiImageLayout::ColorAttachment,
                .clear = false,
            };
            RhiRenderingInfo renderInfo = {
                .extent = ext,
                .colorAttachments = {&colorAtt, 1},
            };
            cmd->beginRendering(renderInfo);
            debugUI->render(cmd);
            cmd->endRendering();
        });

    frameGraph.compile();

    auto* cmd = cmdBuffers[*index];
    cmd->reset();
    cmd->begin();

    std::array<RhiBarrierDesc, 2> preBarriers = {{
        {.texture = swapchain->image(*index), .oldLayout = RhiImageLayout::Undefined, .newLayout = RhiImageLayout::ColorAttachment},
        {.texture = swapchain->depthImage(), .oldLayout = RhiImageLayout::Undefined, .newLayout = RhiImageLayout::DepthStencilAttachment},
    }};
    cmd->pipelineBarrier(preBarriers);

    frameGraph.execute(cmd);

    std::array<RhiBarrierDesc, 1> postBarriers = {{
        {.texture = swapchain->image(*index), .oldLayout = RhiImageLayout::ColorAttachment, .newLayout = RhiImageLayout::PresentSrc},
    }};
    cmd->pipelineBarrier(postBarriers);

    cmd->end();

    RhiSubmitInfo submitInfo = {
        .waitSemaphore = imageAvailableSemaphores[currentFrame],
        .signalSemaphore = renderFinishedSemaphores[currentFrame],
        .fence = inflightFences[currentFrame],
    };
    device->submitCommandBuffer(cmd, submitInfo);
    device->present(swapchain, renderFinishedSemaphores[currentFrame], *index);

    currentFrame = (currentFrame + 1) % swapchain->imageCount();
}

auto Renderer::destroy() -> void {
    device->waitIdle();

    debugUI->shutdown();
    debugUI.reset();

    for (auto* ds : descriptorSets) {
        delete ds;
    }
    if (descriptorPool) {
        device->destroyDescriptorPool(descriptorPool);
    }
    device->destroyDescriptorSetLayout(descriptorSetLayout);

    for (auto* ds : debugDescriptorSets) {
        delete ds;
    }
    device->destroyDescriptorPool(debugDescriptorPool);
    device->destroyDescriptorSetLayout(debugDescriptorSetLayout);

    for (uint32_t i = 0; i < swapchain->imageCount(); i++) {
        device->unmapBuffer(uniformBuffers[i]);
        device->destroyBuffer(uniformBuffers[i]);
        device->unmapBuffer(debugVertexBuffers[i]);
        device->destroyBuffer(debugVertexBuffers[i]);
    }

    device->destroyPipeline(pipeline);
    device->destroyShaderModule(vertShader);
    device->destroyShaderModule(fragShader);

    device->destroyPipeline(debugLinePipeline);
    device->destroyShaderModule(debugVertShader);
    device->destroyShaderModule(debugFragShader);

    device->destroySampler(textureSampler);
    device->destroyTexture(fallbackTexture);

    for (auto& [idx, cached] : meshCache) {
        device->destroyBuffer(cached.vertexBuffer);
        device->destroyBuffer(cached.indexBuffer);
    }
    for (auto& [idx, cached] : textureCache) {
        device->destroyTexture(cached.texture);
    }
    meshCache.clear();
    textureCache.clear();

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
