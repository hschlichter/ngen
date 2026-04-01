#include "renderervulkan.h"
#include "types.h"
#include "camera.h"

#include <SDL3/SDL.h>
#include <vulkan/vk_enum_string_helper.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>

int RendererVulkan::init(SDL_Window* window) {
    if (dev.init(window)) return 1;
    if (swapchain.init(dev, window)) return 1;

    VkResult result;

    vertShader = dev.loadShaderModule("shaders/triangle.vert.spv");
    fragShader = dev.loadShaderModule("shaders/triangle.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertShader,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragShader,
            .pName = "main",
        }
    };

    VkVertexInputBindingDescription bindingDesc = {
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    VkVertexInputAttributeDescription attrDescs[] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, position) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, color) },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, texCoord) },
    };

    VkPipelineVertexInputStateCreateInfo vertexInputState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bindingDesc,
        .vertexAttributeDescriptionCount = 4,
        .pVertexAttributeDescriptions = attrDescs,
    };

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    VkViewport viewport = { 0, 0, (float)swapchain.extent.width, (float)swapchain.extent.height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, swapchain.extent };
    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    VkPipelineRasterizationStateCreateInfo rasterizationState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisampleState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depthStencilState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
    };

    VkPipelineColorBlendAttachmentState colorBlendAttachment = {
        .blendEnable = VK_FALSE,
        .colorWriteMask = 0xF,
    };

    VkPipelineColorBlendStateCreateInfo colorBlendState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &colorBlendAttachment,
    };

    VkDescriptorSetLayoutBinding layoutBindings[] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
    };

    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = layoutBindings,
    };
    result = vkCreateDescriptorSetLayout(dev.device, &descriptorSetLayoutInfo, NULL, &descriptorSetLayout);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDescriptorSetLayout failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    VkPushConstantRange pushConstRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(glm::mat4),
    };

    VkPipelineLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptorSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstRange,
    };

    result = vkCreatePipelineLayout(dev.device, &layoutInfo, NULL, &pipelineLayout);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreatePipelineLayout failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    VkGraphicsPipelineCreateInfo graphicsPipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertexInputState,
        .pInputAssemblyState = &inputAssemblyState,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizationState,
        .pMultisampleState = &multisampleState,
        .pDepthStencilState = &depthStencilState,
        .pColorBlendState = &colorBlendState,
        .layout = pipelineLayout,
        .renderPass = swapchain.renderPass,
    };

    result = vkCreateGraphicsPipelines(dev.device, VK_NULL_HANDLE, 1, &graphicsPipelineInfo, NULL, &pipeline);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateGraphicsPipelines failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    uniformBuffers.resize(swapchain.imageCount);
    uniformBuffersMemory.resize(swapchain.imageCount);
    uniformBuffersMapped.resize(swapchain.imageCount);

    for (uint32_t i = 0; i < swapchain.imageCount; i++) {
        if (dev.createBuffer(sizeof(UniformBufferObject),
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &uniformBuffers[i], &uniformBuffersMemory[i])) return 1;
        vkMapMemory(dev.device, uniformBuffersMemory[i], 0, sizeof(UniformBufferObject), 0, &uniformBuffersMapped[i]);
    }

    cmdBuffers.resize(swapchain.imageCount);
    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev.cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = swapchain.imageCount,
    };
    result = vkAllocateCommandBuffers(dev.device, &allocInfo, cmdBuffers.data());
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateCommandBuffers failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    imageAvailableSemaphores.resize(swapchain.imageCount);
    renderFinishedSemaphores.resize(swapchain.imageCount);
    inflightFences.resize(swapchain.imageCount);

    VkSemaphoreCreateInfo semInfo = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fenceInfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };

    for (uint32_t i = 0; i < swapchain.imageCount; i++) {
        result = vkCreateSemaphore(dev.device, &semInfo, NULL, &imageAvailableSemaphores[i]);
        if (result != VK_SUCCESS) { fprintf(stderr, "vkCreateSemaphore failed: %s(%d)\n", string_VkResult(result), result); return 1; }
        result = vkCreateSemaphore(dev.device, &semInfo, NULL, &renderFinishedSemaphores[i]);
        if (result != VK_SUCCESS) { fprintf(stderr, "vkCreateSemaphore failed: %s(%d)\n", string_VkResult(result), result); return 1; }
        result = vkCreateFence(dev.device, &fenceInfo, NULL, &inflightFences[i]);
        if (result != VK_SUCCESS) { fprintf(stderr, "vkCreateFence failed: %s(%d)\n", string_VkResult(result), result); return 1; }
    }

    return 0;
}

void RendererVulkan::uploadScene(const Scene& scene) {

    gpuMeshes.resize(scene.meshes.size());

    {
        VkSamplerCreateInfo samplerInfo = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        };
        vkCreateSampler(dev.device, &samplerInfo, NULL, &textureSampler);
    }

    std::vector<uint8_t> fallbackPixels(64 * 64 * 4);
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            uint8_t c = ((x / 8) + (y / 8)) % 2 ? 255 : 64;
            uint32_t i = (y * 64 + x) * 4;
            fallbackPixels[i] = fallbackPixels[i+1] = fallbackPixels[i+2] = c;
            fallbackPixels[i+3] = 255;
        }

    for (size_t m = 0; m < scene.meshes.size(); m++) {
        const MeshData& md = scene.meshes[m];
        GpuMesh& gm = gpuMeshes[m];
        gm.transform = md.transform;
        gm.indexCount = (uint32_t)md.indices.size();

        VkDeviceSize vbSize = md.vertices.size() * sizeof(Vertex);
        VkBuffer vStaging; VkDeviceMemory vStagingMem;
        dev.createBuffer(vbSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vStaging, &vStagingMem);
        void* data;
        vkMapMemory(dev.device, vStagingMem, 0, vbSize, 0, &data);
        memcpy(data, md.vertices.data(), vbSize);
        vkUnmapMemory(dev.device, vStagingMem);
        dev.createBuffer(vbSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &gm.vertexBuffer, &gm.vertexMemory);
        dev.copyBuffer(vStaging, gm.vertexBuffer, vbSize);
        vkDestroyBuffer(dev.device, vStaging, NULL); vkFreeMemory(dev.device, vStagingMem, NULL);

        VkDeviceSize ibSize = md.indices.size() * sizeof(uint32_t);
        VkBuffer iStaging; VkDeviceMemory iStagingMem;
        dev.createBuffer(ibSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &iStaging, &iStagingMem);
        vkMapMemory(dev.device, iStagingMem, 0, ibSize, 0, &data);
        memcpy(data, md.indices.data(), ibSize);
        vkUnmapMemory(dev.device, iStagingMem);
        dev.createBuffer(ibSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &gm.indexBuffer, &gm.indexMemory);
        dev.copyBuffer(iStaging, gm.indexBuffer, ibSize);
        vkDestroyBuffer(dev.device, iStaging, NULL); vkFreeMemory(dev.device, iStagingMem, NULL);

        uint32_t tw, th; const uint8_t* texPtr;
        if (!md.texPixels.empty()) { tw = md.texWidth; th = md.texHeight; texPtr = md.texPixels.data(); }
        else { tw = 64; th = 64; texPtr = fallbackPixels.data(); }
        uint32_t texSize = tw * th * 4;

        VkBuffer tStaging; VkDeviceMemory tStagingMem;
        dev.createBuffer(texSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &tStaging, &tStagingMem);
        vkMapMemory(dev.device, tStagingMem, 0, texSize, 0, &data);
        memcpy(data, texPtr, texSize);
        vkUnmapMemory(dev.device, tStagingMem);

        VkImageCreateInfo texImageInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_SRGB, .extent = { tw, th, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        vkCreateImage(dev.device, &texImageInfo, NULL, &gm.textureImage);
        VkMemoryRequirements texMemReqs;
        vkGetImageMemoryRequirements(dev.device, gm.textureImage, &texMemReqs);
        VkMemoryAllocateInfo texAllocInfo = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = texMemReqs.size,
            .memoryTypeIndex = dev.findMemoryType(texMemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };
        vkAllocateMemory(dev.device, &texAllocInfo, NULL, &gm.textureMemory);
        vkBindImageMemory(dev.device, gm.textureImage, gm.textureMemory, 0);

        dev.transitionImageLayout(gm.textureImage,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        {
            VkCommandBufferAllocateInfo ca = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = dev.cmdPool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
            VkCommandBuffer cmd; vkAllocateCommandBuffers(dev.device, &ca, &cmd);
            VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
            vkBeginCommandBuffer(cmd, &bi);
            VkBufferImageCopy region = { .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
                .imageExtent = { tw, th, 1 } };
            vkCmdCopyBufferToImage(cmd, tStaging, gm.textureImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
            vkQueueSubmit(dev.graphicsQueue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(dev.graphicsQueue);
            vkFreeCommandBuffers(dev.device, dev.cmdPool, 1, &cmd);
        }
        dev.transitionImageLayout(gm.textureImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkDestroyBuffer(dev.device, tStaging, NULL); vkFreeMemory(dev.device, tStagingMem, NULL);

        VkImageViewCreateInfo tvInfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = gm.textureImage, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_SRGB,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 } };
        vkCreateImageView(dev.device, &tvInfo, NULL, &gm.textureView);
    }

    uint32_t meshCount = (uint32_t)gpuMeshes.size();
    uint32_t totalSets = swapchain.imageCount * meshCount;
    VkDescriptorPoolSize poolSizes[] = {
        { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = totalSets },
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = totalSets },
    };
    VkDescriptorPoolCreateInfo descriptorPoolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = totalSets,
        .poolSizeCount = 2,
        .pPoolSizes = poolSizes,
    };
    vkCreateDescriptorPool(dev.device, &descriptorPoolInfo, NULL, &descriptorPool);

    std::vector<VkDescriptorSetLayout> dsLayouts(totalSets, descriptorSetLayout);
    descriptorSets.resize(totalSets);
    VkDescriptorSetAllocateInfo descriptorAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = totalSets,
        .pSetLayouts = dsLayouts.data(),
    };
    vkAllocateDescriptorSets(dev.device, &descriptorAllocInfo, descriptorSets.data());

    for (uint32_t i = 0; i < swapchain.imageCount; i++) {
        for (uint32_t m = 0; m < meshCount; m++) {
            VkDescriptorBufferInfo bufInfo = {
                .buffer = uniformBuffers[i],
                .offset = 0,
                .range = sizeof(UniformBufferObject),
            };
            VkDescriptorImageInfo imgInfo = {
                .sampler = textureSampler,
                .imageView = gpuMeshes[m].textureView,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            VkWriteDescriptorSet writes[] = {
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = descriptorSets[i * meshCount + m],
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    .pBufferInfo = &bufInfo,
                },
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = descriptorSets[i * meshCount + m],
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &imgInfo,
                },
            };
            vkUpdateDescriptorSets(dev.device, 2, writes, 0, NULL);
        }
    }
}

void RendererVulkan::draw(const Camera& camera, SDL_Window* window) {

    VkResult result;

    result = vkWaitForFences(dev.device, 1, &inflightFences[currentFrame], VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) { fprintf(stderr, "vkWaitForFences failed: %s(%d)\n", string_VkResult(result), result); return; }

    uint32_t index;
    result = vkAcquireNextImageKHR(dev.device, swapchain.swapchain, UINT64_MAX, imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &index);
    if (result != VK_SUCCESS) { fprintf(stderr, "vkAcquireNextImageKHR failed: %s(%d)\n", string_VkResult(result), result); return; }

    result = vkResetFences(dev.device, 1, &inflightFences[currentFrame]);
    if (result != VK_SUCCESS) { fprintf(stderr, "vkResetFences failed: %s(%d)\n", string_VkResult(result), result); return; }

    int winW, winH;
    SDL_GetWindowSizeInPixels(window, &winW, &winH);
    float aspect = (float)winW / (float)winH;
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
    proj[1][1] *= -1.0f;
    UniformBufferObject ubo = {
        .view = camera.viewMatrix(),
        .proj = proj,
    };
    memcpy(uniformBuffersMapped[index], &ubo, sizeof(ubo));

    vkResetCommandBuffer(cmdBuffers[index], 0);

    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmdBuffers[index], &begin);

    VkClearValue clearValues[2] = {};
    clearValues[0].color = { { 0.1f, 0.1f, 0.1f, 1.0f } };
    clearValues[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo passBeginInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = swapchain.renderPass,
        .framebuffer = swapchain.framebuffers[index],
        .renderArea = { { 0, 0 }, swapchain.extent },
        .clearValueCount = 2,
        .pClearValues = clearValues,
    };

    vkCmdBeginRenderPass(cmdBuffers[index], &passBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmdBuffers[index], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    uint32_t meshCount = (uint32_t)gpuMeshes.size();
    for (uint32_t m = 0; m < meshCount; m++) {
        GpuMesh& gm = gpuMeshes[m];
        glm::mat4 model = gm.transform;
        vkCmdPushConstants(cmdBuffers[index], pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &model);
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(cmdBuffers[index], 0, 1, &gm.vertexBuffer, offsets);
        vkCmdBindIndexBuffer(cmdBuffers[index], gm.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(cmdBuffers[index], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
            &descriptorSets[index * meshCount + m], 0, NULL);
        vkCmdDrawIndexed(cmdBuffers[index], gm.indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(cmdBuffers[index]);
    vkEndCommandBuffer(cmdBuffers[index]);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &imageAvailableSemaphores[currentFrame],
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmdBuffers[index],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &renderFinishedSemaphores[currentFrame],
    };

    vkQueueSubmit(dev.graphicsQueue, 1, &submit, inflightFences[currentFrame]);

    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &renderFinishedSemaphores[currentFrame],
        .swapchainCount = 1,
        .pSwapchains = &swapchain.swapchain,
        .pImageIndices = &index,
    };

    vkQueuePresentKHR(dev.graphicsQueue, &present);
    currentFrame = (currentFrame + 1) % swapchain.imageCount;
}

void RendererVulkan::destroy() {
    vkDeviceWaitIdle(dev.device);

    vkDestroyDescriptorPool(dev.device, descriptorPool, NULL);
    vkDestroyDescriptorSetLayout(dev.device, descriptorSetLayout, NULL);
    for (uint32_t i = 0; i < swapchain.imageCount; i++) {
        vkUnmapMemory(dev.device, uniformBuffersMemory[i]);
        vkDestroyBuffer(dev.device, uniformBuffers[i], NULL);
        vkFreeMemory(dev.device, uniformBuffersMemory[i], NULL);
    }

    vkDestroyPipeline(dev.device, pipeline, NULL);
    vkDestroyPipelineLayout(dev.device, pipelineLayout, NULL);

    vkDestroyShaderModule(dev.device, vertShader, NULL);
    vkDestroyShaderModule(dev.device, fragShader, NULL);

    vkDestroySampler(dev.device, textureSampler, NULL);
    for (auto& gm : gpuMeshes) {
        vkDestroyImageView(dev.device, gm.textureView, NULL);
        vkDestroyImage(dev.device, gm.textureImage, NULL);
        vkFreeMemory(dev.device, gm.textureMemory, NULL);
        vkDestroyBuffer(dev.device, gm.vertexBuffer, NULL);
        vkFreeMemory(dev.device, gm.vertexMemory, NULL);
        vkDestroyBuffer(dev.device, gm.indexBuffer, NULL);
        vkFreeMemory(dev.device, gm.indexMemory, NULL);
    }

    for (uint32_t i = 0; i < swapchain.imageCount; i++) {
        vkDestroySemaphore(dev.device, imageAvailableSemaphores[i], NULL);
        vkDestroySemaphore(dev.device, renderFinishedSemaphores[i], NULL);
        vkDestroyFence(dev.device, inflightFences[i], NULL);
    }

    swapchain.destroy(dev.device);
    dev.destroy();
}
