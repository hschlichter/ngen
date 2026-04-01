#include "rhidevicevulkan.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

VkBufferUsageFlags RhiDeviceVulkan::toVkBufferUsage(RhiBufferUsage usage) {
    VkBufferUsageFlags flags = 0;
    if (usage & RhiBufferUsage::TransferSrc) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (usage & RhiBufferUsage::TransferDst) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (usage & RhiBufferUsage::Vertex)      flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (usage & RhiBufferUsage::Index)       flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (usage & RhiBufferUsage::Uniform)     flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    return flags;
}

VkMemoryPropertyFlags RhiDeviceVulkan::toVkMemoryProps(RhiMemoryUsage usage) {
    switch (usage) {
        case RhiMemoryUsage::GpuOnly:  return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        case RhiMemoryUsage::CpuToGpu: return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    return 0;
}

VkFormat RhiDeviceVulkan::toVkFormat(RhiFormat format) {
    switch (format) {
        case RhiFormat::R32G32_SFLOAT:    return VK_FORMAT_R32G32_SFLOAT;
        case RhiFormat::R32G32B32_SFLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case RhiFormat::R8G8B8A8_SRGB:    return VK_FORMAT_R8G8B8A8_SRGB;
        case RhiFormat::D32_SFLOAT:       return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkShaderStageFlags RhiDeviceVulkan::toVkShaderStage(RhiShaderStage stage) {
    VkShaderStageFlags flags = 0;
    if ((uint32_t)stage & (uint32_t)RhiShaderStage::Vertex)   flags |= VK_SHADER_STAGE_VERTEX_BIT;
    if ((uint32_t)stage & (uint32_t)RhiShaderStage::Fragment) flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    return flags;
}

uint32_t RhiDeviceVulkan::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    fprintf(stderr, "Failed to find suitable memory type\n");
    return UINT32_MAX;
}

void RhiDeviceVulkan::transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    VkPipelineStageFlags srcStage, dstStage;
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &barrier);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue);
    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
}

int RhiDeviceVulkan::init(SDL_Window* window) {
    uint32_t apiVersion = VK_API_VERSION_1_0;
    VkResult result = vkEnumerateInstanceVersion(&apiVersion);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkEnumerateInstanceVersion failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    printf("Vulkan API version: %d.%d.%d\n", VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion), VK_API_VERSION_PATCH(apiVersion));

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "ngen",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "Custom Engine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = apiVersion,
    };

    uint32_t extensionsCount = 0;
    char const* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionsCount);
    for (uint32_t i = 0; i < extensionsCount; i++) {
        printf("%s\n", extensions[i]);
    }

    const char* validationLayers[] = {
        "VK_LAYER_KHRONOS_validation",
    };
    uint32_t validationLayersCount = 1;

    VkInstanceCreateInfo instanceCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = NULL,
#ifdef __APPLE__
        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
#else
        .flags = 0,
#endif
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = validationLayersCount,
        .ppEnabledLayerNames = validationLayers,
        .enabledExtensionCount = extensionsCount,
        .ppEnabledExtensionNames = extensions,
    };

    result = vkCreateInstance(&instanceCreateInfo, NULL, &instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    if (!SDL_Vulkan_CreateSurface(window, instance, NULL, &surface)) {
        fprintf(stderr, "SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return 1;
    }

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkEnumeratePhysicalDevices failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());

    for (uint32_t i = 0; i < deviceCount; i++) {
        uint32_t queueCount;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[i], &queueCount, NULL);

        std::vector<VkQueueFamilyProperties> props(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[i], &queueCount, props.data());

        for (uint32_t j = 0; j < queueCount; j++) {
            uint32_t presentSupport;
            result = vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevices[i], j, surface, &presentSupport);
            if (result != VK_SUCCESS) {
                fprintf(stderr, "vkGetPhysicalDeviceSurfaceSupportKHR failed: %s(%d)\n", string_VkResult(result), result);
                return 1;
            }

            if ((props[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentSupport) {
                physicalDevice = physicalDevices[i];
                queueFamilyIndex = j;
                break;
            }
        }

        if (physicalDevice != VK_NULL_HANDLE) {
            break;
        }
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queueFamilyIndex,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };

    const char* deviceExtensions[] = {
        "VK_KHR_swapchain",
#ifdef __APPLE__
        "VK_KHR_portability_subset",
#endif
    };
    uint32_t deviceExtensionCount = sizeof(deviceExtensions) / sizeof(deviceExtensions[0]);

    VkDeviceCreateInfo deviceCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueCreateInfo,
        .enabledExtensionCount = deviceExtensionCount,
        .ppEnabledExtensionNames = deviceExtensions,
    };

    result = vkCreateDevice(physicalDevice, &deviceCreateInfo, NULL, &device);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDevice failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    vkGetDeviceQueue(device, queueFamilyIndex, 0, &graphicsQueue);

    VkCommandPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamilyIndex,
    };
    result = vkCreateCommandPool(device, &poolInfo, NULL, &cmdPool);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateCommandPool failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    return 0;
}

void RhiDeviceVulkan::destroy() {
    vkDestroyCommandPool(device, cmdPool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyInstance(instance, NULL);
}

void RhiDeviceVulkan::waitIdle() {
    vkDeviceWaitIdle(device);
}

RhiSwapchain* RhiDeviceVulkan::createSwapchain(SDL_Window* window) {
    auto* sc = new RhiSwapchainVulkan();
    if (sc->init(physicalDevice, device, surface, queueFamilyIndex, window)) {
        delete sc;
        return nullptr;
    }
    return sc;
}

RhiBuffer* RhiDeviceVulkan::createBuffer(const RhiBufferDesc& desc) {
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = desc.size,
        .usage = toVkBufferUsage(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    auto* buf = new RhiBufferVulkan();
    VkResult result = vkCreateBuffer(device, &bufferInfo, NULL, &buf->buffer);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateBuffer failed: %s(%d)\n", string_VkResult(result), result);
        delete buf;
        return nullptr;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, buf->buffer, &memReqs);

    uint32_t memTypeIndex = findMemoryType(memReqs.memoryTypeBits, toVkMemoryProps(desc.memory));
    if (memTypeIndex == UINT32_MAX) {
        vkDestroyBuffer(device, buf->buffer, NULL);
        delete buf;
        return nullptr;
    }

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memTypeIndex,
    };
    result = vkAllocateMemory(device, &allocInfo, NULL, &buf->memory);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateMemory failed: %s(%d)\n", string_VkResult(result), result);
        vkDestroyBuffer(device, buf->buffer, NULL);
        delete buf;
        return nullptr;
    }

    result = vkBindBufferMemory(device, buf->buffer, buf->memory, 0);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkBindBufferMemory failed: %s(%d)\n", string_VkResult(result), result);
        vkFreeMemory(device, buf->memory, NULL);
        vkDestroyBuffer(device, buf->buffer, NULL);
        delete buf;
        return nullptr;
    }

    return buf;
}

RhiTexture* RhiDeviceVulkan::createTexture(const RhiTextureDesc& desc) {
    auto* tex = new RhiTextureVulkan();

    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = toVkFormat(desc.format),
        .extent = { desc.width, desc.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult result = vkCreateImage(device, &imageInfo, NULL, &tex->image);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateImage failed: %s(%d)\n", string_VkResult(result), result);
        delete tex;
        return nullptr;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, tex->image, &memReqs);
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    vkAllocateMemory(device, &allocInfo, NULL, &tex->memory);
    vkBindImageMemory(device, tex->image, tex->memory, 0);

    if (desc.initialData && desc.initialDataSize > 0) {
        RhiBufferDesc stagingDesc = {
            .size = desc.initialDataSize,
            .usage = RhiBufferUsage::TransferSrc,
            .memory = RhiMemoryUsage::CpuToGpu,
        };
        RhiBuffer* staging = createBuffer(stagingDesc);
        auto* stagingVk = static_cast<RhiBufferVulkan*>(staging);

        void* data;
        vkMapMemory(device, stagingVk->memory, 0, desc.initialDataSize, 0, &data);
        memcpy(data, desc.initialData, desc.initialDataSize);
        vkUnmapMemory(device, stagingVk->memory);

        transitionImageLayout(tex->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        {
            VkCommandBufferAllocateInfo ca = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = cmdPool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            VkCommandBuffer cmd;
            vkAllocateCommandBuffers(device, &ca, &cmd);
            VkCommandBufferBeginInfo bi = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };
            vkBeginCommandBuffer(cmd, &bi);
            VkBufferImageCopy region = {
                .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
                .imageExtent = { desc.width, desc.height, 1 },
            };
            vkCmdCopyBufferToImage(cmd, stagingVk->buffer, tex->image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo si = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &cmd,
            };
            vkQueueSubmit(graphicsQueue, 1, &si, VK_NULL_HANDLE);
            vkQueueWaitIdle(graphicsQueue);
            vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
        }

        transitionImageLayout(tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        destroyBuffer(staging);
    }

    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = tex->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = toVkFormat(desc.format),
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCreateImageView(device, &viewInfo, NULL, &tex->view);

    return tex;
}

RhiSampler* RhiDeviceVulkan::createSampler(const RhiSamplerDesc& desc) {
    (void)desc;
    auto* sampler = new RhiSamplerVulkan();
    VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
    };
    vkCreateSampler(device, &samplerInfo, NULL, &sampler->sampler);
    return sampler;
}

RhiShaderModule* RhiDeviceVulkan::createShaderModule(const char* filepath) {
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        fprintf(stderr, "Failed to open shader file: %s\n", filepath);
        return nullptr;
    }

    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);
    rewind(file);

    uint32_t* code = (uint32_t*)malloc(size);
    fread(code, 1, size, file);
    fclose(file);

    VkShaderModuleCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = code,
    };

    auto* sm = new RhiShaderModuleVulkan();
    VkResult result = vkCreateShaderModule(device, &createInfo, NULL, &sm->module);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateShaderModule failed: %s(%d)\n", string_VkResult(result), result);
        free(code);
        delete sm;
        return nullptr;
    }

    printf("Loaded shader: %s\n", filepath);
    free(code);
    return sm;
}

RhiPipeline* RhiDeviceVulkan::createGraphicsPipeline(const RhiGraphicsPipelineDesc& desc) {
    auto* vertMod = static_cast<RhiShaderModuleVulkan*>(desc.vertexShader);
    auto* fragMod = static_cast<RhiShaderModuleVulkan*>(desc.fragmentShader);
    auto* dsLayout = static_cast<RhiDescriptorSetLayoutVulkan*>(desc.descriptorSetLayout);
    auto* rp = static_cast<RhiRenderPassVulkan*>(desc.renderPass);

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertMod->module,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragMod->module,
            .pName = "main",
        }
    };

    VkVertexInputBindingDescription bindingDesc = {
        .binding = 0,
        .stride = desc.vertexStride,
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    std::vector<VkVertexInputAttributeDescription> attrDescs(desc.vertexAttributeCount);
    for (uint32_t i = 0; i < desc.vertexAttributeCount; i++) {
        attrDescs[i] = {
            .location = desc.vertexAttributes[i].location,
            .binding = desc.vertexAttributes[i].binding,
            .format = toVkFormat(desc.vertexAttributes[i].format),
            .offset = desc.vertexAttributes[i].offset,
        };
    }

    VkPipelineVertexInputStateCreateInfo vertexInputState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bindingDesc,
        .vertexAttributeDescriptionCount = desc.vertexAttributeCount,
        .pVertexAttributeDescriptions = attrDescs.data(),
    };

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    VkViewport viewport = { 0, 0, (float)desc.viewportExtent.width, (float)desc.viewportExtent.height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { desc.viewportExtent.width, desc.viewportExtent.height } };
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

    VkPushConstantRange pushConstRange = {
        .stageFlags = toVkShaderStage(desc.pushConstant.stage),
        .offset = desc.pushConstant.offset,
        .size = desc.pushConstant.size,
    };

    VkPipelineLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &dsLayout->layout,
        .pushConstantRangeCount = desc.pushConstant.size > 0 ? 1u : 0u,
        .pPushConstantRanges = desc.pushConstant.size > 0 ? &pushConstRange : nullptr,
    };

    auto* pip = new RhiPipelineVulkan();

    VkResult result = vkCreatePipelineLayout(device, &layoutInfo, NULL, &pip->layout);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreatePipelineLayout failed: %s(%d)\n", string_VkResult(result), result);
        delete pip;
        return nullptr;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo = {
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
        .layout = pip->layout,
        .renderPass = rp->renderPass,
    };

    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pip->pipeline);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateGraphicsPipelines failed: %s(%d)\n", string_VkResult(result), result);
        vkDestroyPipelineLayout(device, pip->layout, NULL);
        delete pip;
        return nullptr;
    }

    return pip;
}

RhiDescriptorSetLayout* RhiDeviceVulkan::createDescriptorSetLayout(
    const RhiDescriptorBinding* bindings, uint32_t count) {

    std::vector<VkDescriptorSetLayoutBinding> vkBindings(count);
    for (uint32_t i = 0; i < count; i++) {
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (bindings[i].type == RhiDescriptorType::CombinedImageSampler)
            type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

        vkBindings[i] = {
            .binding = bindings[i].binding,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = toVkShaderStage(bindings[i].stage),
        };
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = count,
        .pBindings = vkBindings.data(),
    };

    auto* layout = new RhiDescriptorSetLayoutVulkan();
    VkResult result = vkCreateDescriptorSetLayout(device, &layoutInfo, NULL, &layout->layout);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDescriptorSetLayout failed: %s(%d)\n", string_VkResult(result), result);
        delete layout;
        return nullptr;
    }
    return layout;
}

RhiDescriptorPool* RhiDeviceVulkan::createDescriptorPool(
    uint32_t maxSets, const RhiDescriptorBinding* bindings, uint32_t bindingCount) {

    std::vector<VkDescriptorPoolSize> poolSizes(bindingCount);
    for (uint32_t i = 0; i < bindingCount; i++) {
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (bindings[i].type == RhiDescriptorType::CombinedImageSampler)
            type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[i] = { .type = type, .descriptorCount = maxSets };
    }

    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = maxSets,
        .poolSizeCount = bindingCount,
        .pPoolSizes = poolSizes.data(),
    };

    auto* pool = new RhiDescriptorPoolVulkan();
    vkCreateDescriptorPool(device, &poolInfo, NULL, &pool->pool);
    return pool;
}

std::vector<RhiDescriptorSet*> RhiDeviceVulkan::allocateDescriptorSets(
    RhiDescriptorPool* pool, RhiDescriptorSetLayout* layout, uint32_t count) {

    auto* vkPool = static_cast<RhiDescriptorPoolVulkan*>(pool);
    auto* vkLayout = static_cast<RhiDescriptorSetLayoutVulkan*>(layout);

    std::vector<VkDescriptorSetLayout> layouts(count, vkLayout->layout);
    std::vector<VkDescriptorSet> vkSets(count);

    VkDescriptorSetAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = vkPool->pool,
        .descriptorSetCount = count,
        .pSetLayouts = layouts.data(),
    };
    vkAllocateDescriptorSets(device, &allocInfo, vkSets.data());

    std::vector<RhiDescriptorSet*> result(count);
    for (uint32_t i = 0; i < count; i++) {
        auto* ds = new RhiDescriptorSetVulkan();
        ds->set = vkSets[i];
        result[i] = ds;
    }
    return result;
}

void RhiDeviceVulkan::updateDescriptorSet(RhiDescriptorSet* set,
    const RhiDescriptorWrite* writes, uint32_t writeCount) {

    auto* vkSet = static_cast<RhiDescriptorSetVulkan*>(set);

    std::vector<VkWriteDescriptorSet> vkWrites(writeCount);
    std::vector<VkDescriptorBufferInfo> bufInfos(writeCount);
    std::vector<VkDescriptorImageInfo> imgInfos(writeCount);

    for (uint32_t i = 0; i < writeCount; i++) {
        vkWrites[i] = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = vkSet->set,
            .dstBinding = writes[i].binding,
            .descriptorCount = 1,
        };

        if (writes[i].type == RhiDescriptorType::UniformBuffer) {
            auto* buf = static_cast<RhiBufferVulkan*>(writes[i].buffer);
            bufInfos[i] = { .buffer = buf->buffer, .offset = 0, .range = writes[i].bufferRange };
            vkWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            vkWrites[i].pBufferInfo = &bufInfos[i];
        } else {
            auto* tex = static_cast<RhiTextureVulkan*>(writes[i].texture);
            auto* sam = static_cast<RhiSamplerVulkan*>(writes[i].sampler);
            imgInfos[i] = {
                .sampler = sam->sampler,
                .imageView = tex->view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            vkWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            vkWrites[i].pImageInfo = &imgInfos[i];
        }
    }

    vkUpdateDescriptorSets(device, writeCount, vkWrites.data(), 0, NULL);
}

RhiCommandBuffer* RhiDeviceVulkan::createCommandBuffer() {
    auto* cb = new RhiCommandBufferVulkan();
    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkResult result = vkAllocateCommandBuffers(device, &allocInfo, &cb->cmd);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateCommandBuffers failed: %s(%d)\n", string_VkResult(result), result);
        delete cb;
        return nullptr;
    }
    return cb;
}

RhiSemaphore* RhiDeviceVulkan::createSemaphore() {
    auto* sem = new RhiSemaphoreVulkan();
    VkSemaphoreCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkResult result = vkCreateSemaphore(device, &info, NULL, &sem->semaphore);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateSemaphore failed: %s(%d)\n", string_VkResult(result), result);
        delete sem;
        return nullptr;
    }
    return sem;
}

RhiFence* RhiDeviceVulkan::createFence(bool signaled) {
    auto* fence = new RhiFenceVulkan();
    VkFenceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0u,
    };
    VkResult result = vkCreateFence(device, &info, NULL, &fence->fence);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateFence failed: %s(%d)\n", string_VkResult(result), result);
        delete fence;
        return nullptr;
    }
    return fence;
}

void RhiDeviceVulkan::waitForFence(RhiFence* fence) {
    auto* f = static_cast<RhiFenceVulkan*>(fence);
    vkWaitForFences(device, 1, &f->fence, VK_TRUE, UINT64_MAX);
}

void RhiDeviceVulkan::resetFence(RhiFence* fence) {
    auto* f = static_cast<RhiFenceVulkan*>(fence);
    vkResetFences(device, 1, &f->fence);
}

void RhiDeviceVulkan::submitCommandBuffer(RhiCommandBuffer* cmd, const RhiSubmitInfo& info) {
    auto* cb = static_cast<RhiCommandBufferVulkan*>(cmd);
    auto* waitSem = static_cast<RhiSemaphoreVulkan*>(info.waitSemaphore);
    auto* signalSem = static_cast<RhiSemaphoreVulkan*>(info.signalSemaphore);
    auto* fence = static_cast<RhiFenceVulkan*>(info.fence);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = waitSem ? 1u : 0u,
        .pWaitSemaphores = waitSem ? &waitSem->semaphore : nullptr,
        .pWaitDstStageMask = waitSem ? &waitStage : nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb->cmd,
        .signalSemaphoreCount = signalSem ? 1u : 0u,
        .pSignalSemaphores = signalSem ? &signalSem->semaphore : nullptr,
    };

    vkQueueSubmit(graphicsQueue, 1, &submit, fence ? fence->fence : VK_NULL_HANDLE);
}

void RhiDeviceVulkan::present(RhiSwapchain* swapchain, RhiSemaphore* waitSemaphore, uint32_t imageIndex) {
    auto* sc = static_cast<RhiSwapchainVulkan*>(swapchain);
    auto* sem = static_cast<RhiSemaphoreVulkan*>(waitSemaphore);

    VkPresentInfoKHR presentInfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = sem ? 1u : 0u,
        .pWaitSemaphores = sem ? &sem->semaphore : nullptr,
        .swapchainCount = 1,
        .pSwapchains = &sc->swapchain,
        .pImageIndices = &imageIndex,
    };

    vkQueuePresentKHR(graphicsQueue, &presentInfo);
}

void* RhiDeviceVulkan::mapBuffer(RhiBuffer* buffer) {
    auto* buf = static_cast<RhiBufferVulkan*>(buffer);
    void* data;
    vkMapMemory(device, buf->memory, 0, VK_WHOLE_SIZE, 0, &data);
    return data;
}

void RhiDeviceVulkan::unmapBuffer(RhiBuffer* buffer) {
    auto* buf = static_cast<RhiBufferVulkan*>(buffer);
    vkUnmapMemory(device, buf->memory);
}

void RhiDeviceVulkan::copyBuffer(RhiBuffer* src, RhiBuffer* dst, uint64_t size) {
    auto* srcBuf = static_cast<RhiBufferVulkan*>(src);
    auto* dstBuf = static_cast<RhiBufferVulkan*>(dst);

    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &beginInfo);
    VkBufferCopy region = { .size = size };
    vkCmdCopyBuffer(cmd, srcBuf->buffer, dstBuf->buffer, 1, &region);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue);
    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
}

void RhiDeviceVulkan::destroyBuffer(RhiBuffer* buffer) {
    auto* b = static_cast<RhiBufferVulkan*>(buffer);
    vkDestroyBuffer(device, b->buffer, NULL);
    vkFreeMemory(device, b->memory, NULL);
    delete b;
}

void RhiDeviceVulkan::destroyTexture(RhiTexture* texture) {
    auto* t = static_cast<RhiTextureVulkan*>(texture);
    vkDestroyImageView(device, t->view, NULL);
    vkDestroyImage(device, t->image, NULL);
    vkFreeMemory(device, t->memory, NULL);
    delete t;
}

void RhiDeviceVulkan::destroySampler(RhiSampler* sampler) {
    auto* s = static_cast<RhiSamplerVulkan*>(sampler);
    vkDestroySampler(device, s->sampler, NULL);
    delete s;
}

void RhiDeviceVulkan::destroyShaderModule(RhiShaderModule* module) {
    auto* m = static_cast<RhiShaderModuleVulkan*>(module);
    vkDestroyShaderModule(device, m->module, NULL);
    delete m;
}

void RhiDeviceVulkan::destroyPipeline(RhiPipeline* pipeline) {
    auto* p = static_cast<RhiPipelineVulkan*>(pipeline);
    vkDestroyPipeline(device, p->pipeline, NULL);
    vkDestroyPipelineLayout(device, p->layout, NULL);
    delete p;
}

void RhiDeviceVulkan::destroyDescriptorSetLayout(RhiDescriptorSetLayout* layout) {
    auto* l = static_cast<RhiDescriptorSetLayoutVulkan*>(layout);
    vkDestroyDescriptorSetLayout(device, l->layout, NULL);
    delete l;
}

void RhiDeviceVulkan::destroyDescriptorPool(RhiDescriptorPool* pool) {
    auto* p = static_cast<RhiDescriptorPoolVulkan*>(pool);
    vkDestroyDescriptorPool(device, p->pool, NULL);
    delete p;
}

void RhiDeviceVulkan::destroySemaphore(RhiSemaphore* semaphore) {
    auto* s = static_cast<RhiSemaphoreVulkan*>(semaphore);
    vkDestroySemaphore(device, s->semaphore, NULL);
    delete s;
}

void RhiDeviceVulkan::destroyFence(RhiFence* fence) {
    auto* f = static_cast<RhiFenceVulkan*>(fence);
    vkDestroyFence(device, f->fence, NULL);
    delete f;
}

void RhiDeviceVulkan::destroyCommandBuffer(RhiCommandBuffer* cmd) {
    auto* cb = static_cast<RhiCommandBufferVulkan*>(cmd);
    vkFreeCommandBuffers(device, cmdPool, 1, &cb->cmd);
    delete cb;
}
