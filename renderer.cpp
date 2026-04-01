#include "renderer.h"
#include "camera.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "rhiswapchain.h"
#include "types.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>

int Renderer::init(RhiDevice* rhiDevice, SDL_Window* window) {
    device = rhiDevice;

    swapchain = device->createSwapchain(window);
    if (!swapchain) {
        return 1;
    }

    vertShader = device->createShaderModule("shaders/triangle.vert.spv");
    fragShader = device->createShaderModule("shaders/triangle.frag.spv");

    RhiDescriptorBinding bindings[] = {
        {.binding = 0, .type = RhiDescriptorType::UniformBuffer, .stage = RhiShaderStage::Vertex},
        {.binding = 1, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Fragment},
    };
    descriptorSetLayout = device->createDescriptorSetLayout(bindings, 2);

    RhiVertexAttribute vertexAttrs[] = {
        {.location = 0, .binding = 0, .format = RhiFormat::R32G32B32_SFLOAT, .offset = offsetof(Vertex, position)},
        {.location = 1, .binding = 0, .format = RhiFormat::R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal)},
        {.location = 2, .binding = 0, .format = RhiFormat::R32G32B32_SFLOAT, .offset = offsetof(Vertex, color)},
        {.location = 3, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = offsetof(Vertex, texCoord)},
    };

    RhiExtent2D ext = swapchain->extent();
    RhiGraphicsPipelineDesc pipelineDesc = {
        .vertexShader = vertShader,
        .fragmentShader = fragShader,
        .descriptorSetLayout = descriptorSetLayout,
        .renderPass = swapchain->renderPass(),
        .vertexStride = sizeof(Vertex),
        .vertexAttributes = vertexAttrs,
        .vertexAttributeCount = 4,
        .pushConstant = {.stage = RhiShaderStage::Vertex, .offset = 0, .size = sizeof(glm::mat4)},
        .viewportExtent = ext,
    };
    pipeline = device->createGraphicsPipeline(pipelineDesc);
    if (!pipeline) {
        return 1;
    }

    uint32_t imgCount = swapchain->imageCount();

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

    return 0;
}

void Renderer::uploadScene(const Scene& scene) {
    gpuMeshes.resize(scene.meshes.size());

    textureSampler = device->createSampler({});

    std::vector<uint8_t> fallbackPixels(64 * 64 * 4);
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            uint8_t c = ((x / 8) + (y / 8)) % 2 ? 255 : 64;
            uint32_t i = (y * 64 + x) * 4;
            fallbackPixels[i] = fallbackPixels[i + 1] = fallbackPixels[i + 2] = c;
            fallbackPixels[i + 3] = 255;
        }
    }

    for (size_t m = 0; m < scene.meshes.size(); m++) {
        const MeshData& md = scene.meshes[m];
        GpuMesh& gm = gpuMeshes[m];
        gm.transform = md.transform;
        gm.indexCount = (uint32_t) md.indices.size();

        // Vertex buffer
        uint64_t vbSize = md.vertices.size() * sizeof(Vertex);
        RhiBufferDesc stagingDesc = {
            .size = vbSize,
            .usage = RhiBufferUsage::TransferSrc,
            .memory = RhiMemoryUsage::CpuToGpu,
        };
        RhiBuffer* vStaging = device->createBuffer(stagingDesc);
        void* data = device->mapBuffer(vStaging);
        memcpy(data, md.vertices.data(), vbSize);
        device->unmapBuffer(vStaging);

        RhiBufferDesc vbDesc = {
            .size = vbSize,
            .usage = RhiBufferUsage::TransferDst | RhiBufferUsage::Vertex,
            .memory = RhiMemoryUsage::GpuOnly,
        };
        gm.vertexBuffer = device->createBuffer(vbDesc);
        device->copyBuffer(vStaging, gm.vertexBuffer, vbSize);
        device->destroyBuffer(vStaging);

        // Index buffer
        uint64_t ibSize = md.indices.size() * sizeof(uint32_t);
        stagingDesc.size = ibSize;
        RhiBuffer* iStaging = device->createBuffer(stagingDesc);
        data = device->mapBuffer(iStaging);
        memcpy(data, md.indices.data(), ibSize);
        device->unmapBuffer(iStaging);

        RhiBufferDesc ibDesc = {
            .size = ibSize,
            .usage = RhiBufferUsage::TransferDst | RhiBufferUsage::Index,
            .memory = RhiMemoryUsage::GpuOnly,
        };
        gm.indexBuffer = device->createBuffer(ibDesc);
        device->copyBuffer(iStaging, gm.indexBuffer, ibSize);
        device->destroyBuffer(iStaging);

        // Texture
        uint32_t tw, th;
        const uint8_t* texPtr;
        if (!md.texPixels.empty()) {
            tw = md.texWidth;
            th = md.texHeight;
            texPtr = md.texPixels.data();
        } else {
            tw = 64;
            th = 64;
            texPtr = fallbackPixels.data();
        }
        uint32_t texSize = tw * th * 4;

        RhiTextureDesc texDesc = {
            .width = tw,
            .height = th,
            .format = RhiFormat::R8G8B8A8_SRGB,
            .initialData = texPtr,
            .initialDataSize = texSize,
        };
        gm.texture = device->createTexture(texDesc);
    }

    uint32_t meshCount = (uint32_t) gpuMeshes.size();
    uint32_t imgCount = swapchain->imageCount();
    uint32_t totalSets = imgCount * meshCount;

    RhiDescriptorBinding bindings[] = {
        {.binding = 0, .type = RhiDescriptorType::UniformBuffer, .stage = RhiShaderStage::Vertex},
        {.binding = 1, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Fragment},
    };
    descriptorPool = device->createDescriptorPool(totalSets, bindings, 2);
    descriptorSets = device->allocateDescriptorSets(descriptorPool, descriptorSetLayout, totalSets);

    for (uint32_t i = 0; i < imgCount; i++) {
        for (uint32_t m = 0; m < meshCount; m++) {
            RhiDescriptorWrite writes[] = {
                {
                    .binding = 0,
                    .type = RhiDescriptorType::UniformBuffer,
                    .buffer = uniformBuffers[i],
                    .bufferRange = sizeof(UniformBufferObject),
                },
                {
                    .binding = 1,
                    .type = RhiDescriptorType::CombinedImageSampler,
                    .texture = gpuMeshes[m].texture,
                    .sampler = textureSampler,
                },
            };
            device->updateDescriptorSet(descriptorSets[i * meshCount + m], writes, 2);
        }
    }
}

void Renderer::draw(const Camera& camera, SDL_Window* window) {
    device->waitForFence(inflightFences[currentFrame]);

    uint32_t index;
    if (swapchain->acquireNextImage(imageAvailableSemaphores[currentFrame], &index)) {
        return;
    }

    device->resetFence(inflightFences[currentFrame]);

    int winW, winH;
    SDL_GetWindowSizeInPixels(window, &winW, &winH);
    float aspect = (float) winW / (float) winH;
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
    proj[1][1] *= -1.0f;
    UniformBufferObject ubo = {
        .view = camera.viewMatrix(),
        .proj = proj,
    };
    memcpy(uniformBuffersMapped[index], &ubo, sizeof(ubo));

    RhiCommandBuffer* cmd = cmdBuffers[index];
    cmd->reset();
    cmd->begin();

    RhiExtent2D ext = swapchain->extent();
    RhiRenderPassBeginDesc rpDesc = {
        .renderPass = swapchain->renderPass(),
        .framebuffer = swapchain->framebuffer(index),
        .extent = ext,
        .clearColor = {0.1f, 0.1f, 0.1f, 1.0f},
        .clearDepth = 1.0f,
    };
    cmd->beginRenderPass(rpDesc);
    cmd->bindPipeline(pipeline);

    uint32_t meshCount = (uint32_t) gpuMeshes.size();
    for (uint32_t m = 0; m < meshCount; m++) {
        GpuMesh& gm = gpuMeshes[m];
        glm::mat4 model = gm.transform;
        cmd->pushConstants(pipeline, RhiShaderStage::Vertex, 0, sizeof(glm::mat4), &model);
        cmd->bindVertexBuffer(gm.vertexBuffer);
        cmd->bindIndexBuffer(gm.indexBuffer);
        cmd->bindDescriptorSet(pipeline, descriptorSets[index * meshCount + m]);
        cmd->drawIndexed(gm.indexCount, 1, 0, 0, 0);
    }

    cmd->endRenderPass();
    cmd->end();

    RhiSubmitInfo submitInfo = {
        .waitSemaphore = imageAvailableSemaphores[currentFrame],
        .signalSemaphore = renderFinishedSemaphores[currentFrame],
        .fence = inflightFences[currentFrame],
    };
    device->submitCommandBuffer(cmd, submitInfo);
    device->present(swapchain, renderFinishedSemaphores[currentFrame], index);

    currentFrame = (currentFrame + 1) % swapchain->imageCount();
}

void Renderer::destroy() {
    device->waitIdle();

    for (auto* ds : descriptorSets) {
        delete ds;
    }
    device->destroyDescriptorPool(descriptorPool);
    device->destroyDescriptorSetLayout(descriptorSetLayout);

    for (uint32_t i = 0; i < swapchain->imageCount(); i++) {
        device->unmapBuffer(uniformBuffers[i]);
        device->destroyBuffer(uniformBuffers[i]);
    }

    device->destroyPipeline(pipeline);
    device->destroyShaderModule(vertShader);
    device->destroyShaderModule(fragShader);
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

    swapchain->destroy();
    delete swapchain;
}
