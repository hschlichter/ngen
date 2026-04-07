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

    device->waitIdle();

    // Clean up previous upload
    for (auto* ds : descriptorSets) {
        delete ds;
    }
    descriptorSets.clear();
    if (descriptorPool) {
        device->destroyDescriptorPool(descriptorPool);
        descriptorPool = nullptr;
    }
    for (auto& gm : gpuMeshes) {
        if (gm.vertexBuffer) {
            device->destroyBuffer(gm.vertexBuffer);
        }
        if (gm.indexBuffer) {
            device->destroyBuffer(gm.indexBuffer);
        }
        if (gm.texture) {
            device->destroyTexture(gm.texture);
        }
    }
    if (textureSampler) {
        device->destroySampler(textureSampler);
    }

    gpuMeshes.resize(world.meshInstances.size());

    textureSampler = device->createSampler({});

    std::vector<uint8_t> fallbackPixels(static_cast<size_t>(64) * 64 * 4);
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            auto c = ((((x / 8) + (y / 8)) % 2) != 0u) ? (uint8_t) 255 : (uint8_t) 64;
            auto i = (y * 64 + x) * 4;
            fallbackPixels[i] = fallbackPixels[i + 1] = fallbackPixels[i + 2] = c;
            fallbackPixels[i + 3] = 255;
        }
    }

    for (size_t m = 0; m < world.meshInstances.size(); m++) {
        const auto& inst = world.meshInstances[m];
        const auto* meshData = meshLib.get(inst.mesh);
        const auto* matData = matLib.get(inst.material);
        auto& gm = gpuMeshes[m];
        gm.transform = inst.worldTransform;
        gm.indexCount = meshData ? (uint32_t) meshData->indices.size() : 0;

        if (!meshData) {
            continue;
        }

        // Vertex buffer
        auto vbSize = (uint64_t) (meshData->vertices.size() * sizeof(Vertex));
        RhiBufferDesc stagingDesc = {
            .size = vbSize,
            .usage = RhiBufferUsage::TransferSrc,
            .memory = CpuToGpu,
        };
        auto* vStaging = device->createBuffer(stagingDesc);
        auto* data = device->mapBuffer(vStaging);
        memcpy(data, meshData->vertices.data(), vbSize);
        device->unmapBuffer(vStaging);

        RhiBufferDesc vbDesc = {
            .size = vbSize,
            .usage = RhiBufferUsage::TransferDst | RhiBufferUsage::Vertex,
            .memory = GpuOnly,
        };
        gm.vertexBuffer = device->createBuffer(vbDesc);
        device->copyBuffer(vStaging, gm.vertexBuffer, vbSize);
        device->destroyBuffer(vStaging);

        // Index buffer
        auto ibSize = (uint64_t) (meshData->indices.size() * sizeof(uint32_t));
        stagingDesc.size = ibSize;
        auto* iStaging = device->createBuffer(stagingDesc);
        data = device->mapBuffer(iStaging);
        memcpy(data, meshData->indices.data(), ibSize);
        device->unmapBuffer(iStaging);

        RhiBufferDesc ibDesc = {
            .size = ibSize,
            .usage = RhiBufferUsage::TransferDst | RhiBufferUsage::Index,
            .memory = GpuOnly,
        };
        gm.indexBuffer = device->createBuffer(ibDesc);
        device->copyBuffer(iStaging, gm.indexBuffer, ibSize);
        device->destroyBuffer(iStaging);

        // Texture
        uint32_t tw = 0;
        uint32_t th = 0;
        const uint8_t* texPtr = nullptr;
        if (matData && !matData->texPixels.empty()) {
            tw = matData->texWidth;
            th = matData->texHeight;
            texPtr = matData->texPixels.data();
        } else {
            tw = 64;
            th = 64;
            texPtr = fallbackPixels.data();
        }
        auto texSize = tw * th * 4;

        RhiTextureDesc texDesc = {
            .width = tw,
            .height = th,
            .format = RhiFormat::R8G8B8A8_SRGB,
            .initialData = texPtr,
            .initialDataSize = texSize,
        };
        gm.texture = device->createTexture(texDesc);
    }

    auto meshCount = (uint32_t) gpuMeshes.size();
    auto imgCount = swapchain->imageCount();
    auto totalSets = imgCount * meshCount;

    std::array<RhiDescriptorBinding, 2> poolBindings = {{
        {.binding = 0, .type = UniformBuffer, .stage = RhiShaderStage::Vertex},
        {.binding = 1, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},
    }};
    descriptorPool = device->createDescriptorPool(totalSets, poolBindings);
    descriptorSets = device->allocateDescriptorSets(descriptorPool, descriptorSetLayout, totalSets);

    for (uint32_t i = 0; i < imgCount; i++) {
        for (uint32_t m = 0; m < meshCount; m++) {
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
                    .texture = gpuMeshes[m].texture,
                    .sampler = textureSampler,
                },
            }};
            device->updateDescriptorSet(descriptorSets[(i * meshCount) + m], writes);
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
    auto meshCount = (uint32_t) gpuMeshes.size();

    frameGraph.addPass<ForwardPassData>(
        "ForwardPass",
        [&](FrameGraphBuilder& builder, ForwardPassData& data) {
            data.color = builder.write(colorHandle, FgAccessFlags::ColorAttachment);
            data.depth = builder.write(depthHandle, FgAccessFlags::DepthAttachment);
            builder.setSideEffects(true);
        },
        [this, imageIdx, meshCount, ext](FrameGraphContext& ctx, const ForwardPassData& data) {
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

            for (uint32_t m = 0; m < meshCount; m++) {
                auto& gm = gpuMeshes[m];
                auto model = gm.transform;
                cmd->pushConstants(pipeline, RhiShaderStage::Vertex, 0, sizeof(glm::mat4), &model);
                cmd->bindVertexBuffer(gm.vertexBuffer);
                cmd->bindIndexBuffer(gm.indexBuffer);
                cmd->bindDescriptorSet(pipeline, descriptorSets[(imageIdx * meshCount) + m]);
                cmd->drawIndexed(gm.indexCount, 1, 0, 0, 0);
            }

            cmd->endRendering();
        });

    debugRenderer.addPass(frameGraph, colorHandle, depthHandle, ext, debugData, {
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

    // Transition swapchain image: Undefined -> ColorAttachment (before graph)
    std::array<RhiBarrierDesc, 2> preBarriers = {{
        {.texture = swapchain->image(*index), .oldLayout = RhiImageLayout::Undefined, .newLayout = RhiImageLayout::ColorAttachment},
        {.texture = swapchain->depthImage(), .oldLayout = RhiImageLayout::Undefined, .newLayout = RhiImageLayout::DepthStencilAttachment},
    }};
    cmd->pipelineBarrier(preBarriers);

    frameGraph.execute(cmd);

    // Transition swapchain image: ColorAttachment -> PresentSrc (after graph)
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
    device->destroyDescriptorPool(descriptorPool);
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

    for (auto& gm : gpuMeshes) {
        device->destroyTexture(gm.texture);
        device->destroyBuffer(gm.vertexBuffer);
        device->destroyBuffer(gm.indexBuffer);
    }

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
