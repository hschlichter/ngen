#include "rhidevicevulkan.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <print>
#include <utility>
#include <vector>

auto RhiDeviceVulkan::toVkBufferUsage(RhiBufferUsage usage) -> VkBufferUsageFlags {
    using enum RhiBufferUsage;
    VkBufferUsageFlags flags = 0;
    if (usage & TransferSrc) {
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (usage & TransferDst) {
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if (usage & Vertex) {
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (usage & Index) {
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (usage & Uniform) {
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    return flags;
}

auto RhiDeviceVulkan::toVkMemoryProps(RhiMemoryUsage usage) -> VkMemoryPropertyFlags {
    using enum RhiMemoryUsage;
    switch (usage) {
        case GpuOnly:
            return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        case CpuToGpu:
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    return 0;
}

auto RhiDeviceVulkan::toVkFormat(RhiFormat format) -> VkFormat {
    using enum RhiFormat;
    switch (format) {
        case Undefined:
            return VK_FORMAT_UNDEFINED;
        case R32G32_SFLOAT:
            return VK_FORMAT_R32G32_SFLOAT;
        case R32G32B32_SFLOAT:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case R8G8B8A8_SRGB:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case R8G8B8A8_UNORM:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case B8G8R8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case B8G8R8A8_UNORM:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case R32G32B32A32_SFLOAT:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case D32_SFLOAT:
            return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

auto RhiDeviceVulkan::toVkShaderStage(RhiShaderStage stage) -> VkShaderStageFlags {
    VkShaderStageFlags flags = 0;
    if ((std::to_underlying(stage) & std::to_underlying(RhiShaderStage::Vertex)) != 0u) {
        flags |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if ((std::to_underlying(stage) & std::to_underlying(RhiShaderStage::Fragment)) != 0u) {
        flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    return flags;
}

auto RhiDeviceVulkan::toVkImageUsage(RhiTextureUsage usage) -> VkImageUsageFlags {
    VkImageUsageFlags flags = 0;
    if (usage & RhiTextureUsage::Sampled) {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (usage & RhiTextureUsage::ColorAttachment) {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (usage & RhiTextureUsage::DepthAttachment) {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (usage & RhiTextureUsage::Storage) {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (usage & RhiTextureUsage::TransferSrc) {
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (usage & RhiTextureUsage::TransferDst) {
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    return flags;
}

auto RhiDeviceVulkan::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) -> uint32_t {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (((typeFilter & (1 << i)) != 0u) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    std::println(stderr, "Failed to find suitable memory type");
    return UINT32_MAX;
}

auto RhiDeviceVulkan::transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) -> void {
    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = nullptr;
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
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };

    VkPipelineStageFlags srcStage = 0;
    VkPipelineStageFlags dstStage = 0;
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

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
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

auto RhiDeviceVulkan::init(SDL_Window* window) -> std::expected<void, int> {
    uint32_t apiVersion = VK_API_VERSION_1_0;
    auto result = vkEnumerateInstanceVersion(&apiVersion);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkEnumerateInstanceVersion failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }

    std::println("Vulkan API version: {}.{}.{}", VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion), VK_API_VERSION_PATCH(apiVersion));

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "ngen",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "Custom Engine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = apiVersion,
    };

    uint32_t extensionsCount = 0;
    const auto* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionsCount);
    for (uint32_t i = 0; i < extensionsCount; i++) {
        std::println("{}", extensions[i]);
    }

    const char* validationLayers[] = {
        // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
        "VK_LAYER_KHRONOS_validation",
    };
    uint32_t validationLayersCount = 1;

    VkInstanceCreateInfo instanceCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
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

    result = vkCreateInstance(&instanceCreateInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateInstance failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }

    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        std::println(stderr, "SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
        return std::unexpected(1);
    }

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkEnumeratePhysicalDevices failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }

    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());

    for (uint32_t i = 0; i < deviceCount; i++) {
        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[i], &queueCount, nullptr);

        std::vector<VkQueueFamilyProperties> props(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[i], &queueCount, props.data());

        for (uint32_t j = 0; j < queueCount; j++) {
            uint32_t presentSupport = 0;
            result = vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevices[i], j, surface, &presentSupport);
            if (result != VK_SUCCESS) {
                std::println(stderr, "vkGetPhysicalDeviceSurfaceSupportKHR failed: {}({})", string_VkResult(result), (int) result);
                return std::unexpected(1);
            }

            if (((props[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) && (presentSupport != 0u)) {
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
        // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
        "VK_KHR_swapchain",
#ifdef __APPLE__
        "VK_KHR_portability_subset",
#endif
    };
    auto deviceExtensionCount = (uint32_t) (sizeof(deviceExtensions) / sizeof(deviceExtensions[0]));

    VkPhysicalDeviceSynchronization2Features sync2Features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
        .synchronization2 = VK_TRUE,
    };

    VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
        .pNext = &sync2Features,
        .dynamicRendering = VK_TRUE,
    };

    VkPhysicalDeviceFeatures supportedFeatures = {};
    vkGetPhysicalDeviceFeatures(physicalDevice, &supportedFeatures);
    VkPhysicalDeviceFeatures enabledFeatures = {};
    enabledFeatures.wideLines = supportedFeatures.wideLines;

    VkDeviceCreateInfo deviceCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &dynamicRenderingFeatures,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueCreateInfo,
        .enabledExtensionCount = deviceExtensionCount,
        .ppEnabledExtensionNames = deviceExtensions,
        .pEnabledFeatures = &enabledFeatures,
    };

    result = vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateDevice failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }

    vkGetDeviceQueue(device, queueFamilyIndex, 0, &graphicsQueue);

    VkCommandPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamilyIndex,
    };
    result = vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateCommandPool failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }

    return {};
}

auto RhiDeviceVulkan::destroy() -> void {
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
}

auto RhiDeviceVulkan::waitIdle() -> void {
    vkDeviceWaitIdle(device);
}

auto RhiDeviceVulkan::createSwapchain(SDL_Window* window) -> RhiSwapchain* {
    auto* sc = new RhiSwapchainVulkan();
    if (!sc->init(physicalDevice, device, surface, queueFamilyIndex, window)) {
        delete sc;
        return nullptr;
    }
    return sc;
}

auto RhiDeviceVulkan::createBuffer(const RhiBufferDesc& desc) -> RhiBuffer* {
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = desc.size,
        .usage = toVkBufferUsage(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    auto* buf = new RhiBufferVulkan();
    auto result = vkCreateBuffer(device, &bufferInfo, nullptr, &buf->buffer);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateBuffer failed: {}({})", string_VkResult(result), (int) result);
        delete buf;
        return nullptr;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, buf->buffer, &memReqs);

    auto memTypeIndex = findMemoryType(memReqs.memoryTypeBits, toVkMemoryProps(desc.memory));
    if (memTypeIndex == UINT32_MAX) {
        vkDestroyBuffer(device, buf->buffer, nullptr);
        delete buf;
        return nullptr;
    }

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memTypeIndex,
    };
    result = vkAllocateMemory(device, &allocInfo, nullptr, &buf->memory);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkAllocateMemory failed: {}({})", string_VkResult(result), (int) result);
        vkDestroyBuffer(device, buf->buffer, nullptr);
        delete buf;
        return nullptr;
    }

    result = vkBindBufferMemory(device, buf->buffer, buf->memory, 0);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkBindBufferMemory failed: {}({})", string_VkResult(result), (int) result);
        vkFreeMemory(device, buf->memory, nullptr);
        vkDestroyBuffer(device, buf->buffer, nullptr);
        delete buf;
        return nullptr;
    }

    return buf;
}

auto RhiDeviceVulkan::createTexture(const RhiTextureDesc& desc) -> RhiTexture* {
    auto* tex = new RhiTextureVulkan();

    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = toVkFormat(desc.format),
        .extent = {desc.width, desc.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = toVkImageUsage(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    auto result = vkCreateImage(device, &imageInfo, nullptr, &tex->image);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateImage failed: {}({})", string_VkResult(result), (int) result);
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
    vkAllocateMemory(device, &allocInfo, nullptr, &tex->memory);
    vkBindImageMemory(device, tex->image, tex->memory, 0);

    if ((desc.initialData != nullptr) && desc.initialDataSize > 0) {
        RhiBufferDesc stagingDesc = {
            .size = desc.initialDataSize,
            .usage = RhiBufferUsage::TransferSrc,
            .memory = RhiMemoryUsage::CpuToGpu,
        };
        auto* staging = createBuffer(stagingDesc);
        auto* stagingVk = static_cast<RhiBufferVulkan*>(staging);

        void* data = nullptr;
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
            VkCommandBuffer cmd = nullptr;
            vkAllocateCommandBuffers(device, &ca, &cmd);
            VkCommandBufferBeginInfo bi = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };
            vkBeginCommandBuffer(cmd, &bi);
            VkBufferImageCopy region = {
                .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
                .imageExtent = {desc.width, desc.height, 1},
            };
            vkCmdCopyBufferToImage(cmd, stagingVk->buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
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

        transitionImageLayout(tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        destroyBuffer(staging);
    }

    VkImageAspectFlags aspect = (desc.usage & RhiTextureUsage::DepthAttachment) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = tex->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = toVkFormat(desc.format),
        .subresourceRange =
            {
                .aspectMask = aspect,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    vkCreateImageView(device, &viewInfo, nullptr, &tex->view);

    return tex;
}

auto RhiDeviceVulkan::createSampler(const RhiSamplerDesc& desc) -> RhiSampler* {
    (void) desc;
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
    vkCreateSampler(device, &samplerInfo, nullptr, &sampler->sampler);
    return sampler;
}

auto RhiDeviceVulkan::createShaderModule(const char* filepath) -> RhiShaderModule* {
    auto* file = fopen(filepath, "rb");
    if (file == nullptr) {
        std::println(stderr, "Failed to open shader file: {}", filepath);
        return nullptr;
    }

    fseek(file, 0, SEEK_END);
    auto size = (size_t) ftell(file);
    fseek(file, 0, SEEK_SET);

    auto* code = (uint32_t*) malloc(size);
    fread(code, 1, size, file);
    fclose(file);

    VkShaderModuleCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = code,
    };

    auto* sm = new RhiShaderModuleVulkan();
    auto result = vkCreateShaderModule(device, &createInfo, nullptr, &sm->module);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateShaderModule failed: {}({})", string_VkResult(result), (int) result);
        free(code);
        delete sm;
        return nullptr;
    }

    std::println("Loaded shader: {}", filepath);
    free(code);
    return sm;
}

auto RhiDeviceVulkan::createGraphicsPipeline(const RhiGraphicsPipelineDesc& desc) -> RhiPipeline* {
    auto* vertMod = static_cast<RhiShaderModuleVulkan*>(desc.vertexShader);
    auto* fragMod = static_cast<RhiShaderModuleVulkan*>(desc.fragmentShader);
    auto* dsLayout = static_cast<RhiDescriptorSetLayoutVulkan*>(desc.descriptorSetLayout);

    VkPipelineShaderStageCreateInfo stages[2] = {{
                                                     // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
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
                                                 }};

    VkVertexInputBindingDescription bindingDesc = {
        .binding = 0,
        .stride = desc.vertexStride,
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    auto attrCount = (uint32_t) desc.vertexAttributes.size();
    std::vector<VkVertexInputAttributeDescription> attrDescs(attrCount);
    for (uint32_t i = 0; i < attrCount; i++) {
        attrDescs[i] = {
            .location = desc.vertexAttributes[i].location,
            .binding = desc.vertexAttributes[i].binding,
            .format = toVkFormat(desc.vertexAttributes[i].format),
            .offset = desc.vertexAttributes[i].offset,
        };
    }

    VkPipelineVertexInputStateCreateInfo vertexInputState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = attrCount > 0 ? 1u : 0u,
        .pVertexBindingDescriptions = attrCount > 0 ? &bindingDesc : nullptr,
        .vertexAttributeDescriptionCount = attrCount,
        .pVertexAttributeDescriptions = attrDescs.data(),
    };

    auto vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (desc.topology == RhiPrimitiveTopology::LineList) {
        vkTopology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = vkTopology,
    };

    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = (uint32_t) dynamicStates.size(),
        .pDynamicStates = dynamicStates.data(),
    };

    VkPipelineRasterizationStateCreateInfo rasterizationState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = desc.backfaceCulling ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = desc.lineWidth,
    };

    VkPipelineMultisampleStateCreateInfo multisampleState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depthStencilState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = desc.depthTestEnable ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = desc.depthWriteEnable ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
    };

    auto colorAttachmentCount = (uint32_t) desc.colorFormats.size();
    std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments(colorAttachmentCount,
                                                                           {
                                                                               .blendEnable = VK_FALSE,
                                                                               .colorWriteMask = 0xF,
                                                                           });

    VkPipelineColorBlendStateCreateInfo colorBlendState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = colorAttachmentCount,
        .pAttachments = colorBlendAttachments.data(),
    };

    VkPushConstantRange pushConstRange = {
        .stageFlags = toVkShaderStage(desc.pushConstant.stage),
        .offset = desc.pushConstant.offset,
        .size = desc.pushConstant.size,
    };

    VkPipelineLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = dsLayout != nullptr ? 1u : 0u,
        .pSetLayouts = dsLayout != nullptr ? &dsLayout->layout : nullptr,
        .pushConstantRangeCount = desc.pushConstant.size > 0 ? 1u : 0u,
        .pPushConstantRanges = desc.pushConstant.size > 0 ? &pushConstRange : nullptr,
    };

    auto* pip = new RhiPipelineVulkan();

    auto result = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pip->layout);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreatePipelineLayout failed: {}({})", string_VkResult(result), (int) result);
        delete pip;
        return nullptr;
    }

    std::vector<VkFormat> vkColorFormats;
    vkColorFormats.reserve(desc.colorFormats.size());
    for (auto f : desc.colorFormats) {
        vkColorFormats.push_back(toVkFormat(f));
    }

    VkPipelineRenderingCreateInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = (uint32_t) vkColorFormats.size(),
        .pColorAttachmentFormats = vkColorFormats.data(),
        .depthAttachmentFormat = desc.depthFormat != RhiFormat::Undefined ? toVkFormat(desc.depthFormat) : VK_FORMAT_UNDEFINED,
    };

    VkGraphicsPipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &renderingInfo,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertexInputState,
        .pInputAssemblyState = &inputAssemblyState,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizationState,
        .pMultisampleState = &multisampleState,
        .pDepthStencilState = &depthStencilState,
        .pColorBlendState = &colorBlendState,
        .pDynamicState = &dynamicState,
        .layout = pip->layout,
    };

    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pip->pipeline);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateGraphicsPipelines failed: {}({})", string_VkResult(result), (int) result);
        vkDestroyPipelineLayout(device, pip->layout, nullptr);
        delete pip;
        return nullptr;
    }

    return pip;
}

auto RhiDeviceVulkan::createDescriptorSetLayout(std::span<const RhiDescriptorBinding> bindings) -> RhiDescriptorSetLayout* {
    auto count = (uint32_t) bindings.size();
    std::vector<VkDescriptorSetLayoutBinding> vkBindings(count);
    for (uint32_t i = 0; i < count; i++) {
        auto type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (bindings[i].type == RhiDescriptorType::CombinedImageSampler) {
            type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }

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
    auto result = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout->layout);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateDescriptorSetLayout failed: {}({})", string_VkResult(result), (int) result);
        delete layout;
        return nullptr;
    }
    return layout;
}

auto RhiDeviceVulkan::createDescriptorPool(uint32_t maxSets, std::span<const RhiDescriptorBinding> bindings) -> RhiDescriptorPool* {
    auto bindingCount = (uint32_t) bindings.size();
    std::vector<VkDescriptorPoolSize> poolSizes(bindingCount);
    for (uint32_t i = 0; i < bindingCount; i++) {
        auto type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (bindings[i].type == RhiDescriptorType::CombinedImageSampler) {
            type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        poolSizes[i] = {.type = type, .descriptorCount = maxSets};
    }

    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = maxSets,
        .poolSizeCount = bindingCount,
        .pPoolSizes = poolSizes.data(),
    };

    auto* pool = new RhiDescriptorPoolVulkan();
    vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool->pool);
    return pool;
}

auto RhiDeviceVulkan::allocateDescriptorSets(RhiDescriptorPool* pool, RhiDescriptorSetLayout* layout, uint32_t count) -> std::vector<RhiDescriptorSet*> {

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

auto RhiDeviceVulkan::updateDescriptorSet(RhiDescriptorSet* set, std::span<const RhiDescriptorWrite> writes) -> void {
    auto* vkSet = static_cast<RhiDescriptorSetVulkan*>(set);
    auto writeCount = (uint32_t) writes.size();

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
            bufInfos[i] = {.buffer = buf->buffer, .offset = 0, .range = writes[i].bufferRange};
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

    vkUpdateDescriptorSets(device, writeCount, vkWrites.data(), 0, nullptr);
}

auto RhiDeviceVulkan::createCommandBuffer() -> RhiCommandBuffer* {
    auto* cb = new RhiCommandBufferVulkan();
    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    auto result = vkAllocateCommandBuffers(device, &allocInfo, &cb->cmd);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkAllocateCommandBuffers failed: {}({})", string_VkResult(result), (int) result);
        delete cb;
        return nullptr;
    }
    return cb;
}

auto RhiDeviceVulkan::createSemaphore() -> RhiSemaphore* {
    auto* sem = new RhiSemaphoreVulkan();
    VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    auto result = vkCreateSemaphore(device, &info, nullptr, &sem->semaphore);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateSemaphore failed: {}({})", string_VkResult(result), (int) result);
        delete sem;
        return nullptr;
    }
    return sem;
}

auto RhiDeviceVulkan::createFence(bool signaled) -> RhiFence* {
    auto* fence = new RhiFenceVulkan();
    VkFenceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0u,
    };
    auto result = vkCreateFence(device, &info, nullptr, &fence->fence);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateFence failed: {}({})", string_VkResult(result), (int) result);
        delete fence;
        return nullptr;
    }
    return fence;
}

auto RhiDeviceVulkan::waitForFence(RhiFence* fence) -> void {
    auto* f = static_cast<RhiFenceVulkan*>(fence);
    vkWaitForFences(device, 1, &f->fence, VK_TRUE, UINT64_MAX);
}

auto RhiDeviceVulkan::resetFence(RhiFence* fence) -> void {
    auto* f = static_cast<RhiFenceVulkan*>(fence);
    vkResetFences(device, 1, &f->fence);
}

auto RhiDeviceVulkan::submitCommandBuffer(RhiCommandBuffer* cmd, const RhiSubmitInfo& info) -> void {
    auto* cb = static_cast<RhiCommandBufferVulkan*>(cmd);
    auto* waitSem = static_cast<RhiSemaphoreVulkan*>(info.waitSemaphore);
    auto* signalSem = static_cast<RhiSemaphoreVulkan*>(info.signalSemaphore);
    auto* fence = static_cast<RhiFenceVulkan*>(info.fence);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = (waitSem != nullptr) ? 1u : 0u,
        .pWaitSemaphores = (waitSem != nullptr) ? &waitSem->semaphore : nullptr,
        .pWaitDstStageMask = (waitSem != nullptr) ? &waitStage : nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb->cmd,
        .signalSemaphoreCount = (signalSem != nullptr) ? 1u : 0u,
        .pSignalSemaphores = (signalSem != nullptr) ? &signalSem->semaphore : nullptr,
    };

    vkQueueSubmit(graphicsQueue, 1, &submit, (fence != nullptr) ? fence->fence : VK_NULL_HANDLE);
}

auto RhiDeviceVulkan::present(RhiSwapchain* swapchain, RhiSemaphore* waitSemaphore, uint32_t imageIndex) -> bool {
    auto* sc = static_cast<RhiSwapchainVulkan*>(swapchain);
    auto* sem = static_cast<RhiSemaphoreVulkan*>(waitSemaphore);

    VkPresentInfoKHR presentInfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = (sem != nullptr) ? 1u : 0u,
        .pWaitSemaphores = (sem != nullptr) ? &sem->semaphore : nullptr,
        .swapchainCount = 1,
        .pSwapchains = &sc->swapchain,
        .pImageIndices = &imageIndex,
    };

    auto result = vkQueuePresentKHR(graphicsQueue, &presentInfo);
    return result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_SUBOPTIMAL_KHR;
}

auto RhiDeviceVulkan::mapBuffer(RhiBuffer* buffer) -> void* {
    auto* buf = static_cast<RhiBufferVulkan*>(buffer);
    void* data = nullptr;
    vkMapMemory(device, buf->memory, 0, VK_WHOLE_SIZE, 0, &data);
    return data;
}

auto RhiDeviceVulkan::unmapBuffer(RhiBuffer* buffer) -> void {
    auto* buf = static_cast<RhiBufferVulkan*>(buffer);
    vkUnmapMemory(device, buf->memory);
}

auto RhiDeviceVulkan::copyBuffer(RhiBuffer* src, RhiBuffer* dst, uint64_t size) -> void {
    auto* srcBuf = static_cast<RhiBufferVulkan*>(src);
    auto* dstBuf = static_cast<RhiBufferVulkan*>(dst);

    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = nullptr;
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &beginInfo);
    VkBufferCopy region = {.size = size};
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

auto RhiDeviceVulkan::destroyBuffer(RhiBuffer* buffer) -> void {
    auto* b = static_cast<RhiBufferVulkan*>(buffer);
    vkDestroyBuffer(device, b->buffer, nullptr);
    vkFreeMemory(device, b->memory, nullptr);
    delete b;
}

auto RhiDeviceVulkan::destroyTexture(RhiTexture* texture) -> void {
    auto* t = static_cast<RhiTextureVulkan*>(texture);
    vkDestroyImageView(device, t->view, nullptr);
    vkDestroyImage(device, t->image, nullptr);
    vkFreeMemory(device, t->memory, nullptr);
    delete t;
}

auto RhiDeviceVulkan::destroySampler(RhiSampler* sampler) -> void {
    auto* s = static_cast<RhiSamplerVulkan*>(sampler);
    vkDestroySampler(device, s->sampler, nullptr);
    delete s;
}

auto RhiDeviceVulkan::destroyShaderModule(RhiShaderModule* module) -> void {
    auto* m = static_cast<RhiShaderModuleVulkan*>(module);
    vkDestroyShaderModule(device, m->module, nullptr);
    delete m;
}

auto RhiDeviceVulkan::destroyPipeline(RhiPipeline* pipeline) -> void {
    auto* p = static_cast<RhiPipelineVulkan*>(pipeline);
    vkDestroyPipeline(device, p->pipeline, nullptr);
    vkDestroyPipelineLayout(device, p->layout, nullptr);
    delete p;
}

auto RhiDeviceVulkan::destroyDescriptorSetLayout(RhiDescriptorSetLayout* layout) -> void {
    auto* l = static_cast<RhiDescriptorSetLayoutVulkan*>(layout);
    vkDestroyDescriptorSetLayout(device, l->layout, nullptr);
    delete l;
}

auto RhiDeviceVulkan::destroyDescriptorPool(RhiDescriptorPool* pool) -> void {
    auto* p = static_cast<RhiDescriptorPoolVulkan*>(pool);
    vkDestroyDescriptorPool(device, p->pool, nullptr);
    delete p;
}

auto RhiDeviceVulkan::destroySemaphore(RhiSemaphore* semaphore) -> void {
    auto* s = static_cast<RhiSemaphoreVulkan*>(semaphore);
    vkDestroySemaphore(device, s->semaphore, nullptr);
    delete s;
}

auto RhiDeviceVulkan::destroyFence(RhiFence* fence) -> void {
    auto* f = static_cast<RhiFenceVulkan*>(fence);
    vkDestroyFence(device, f->fence, nullptr);
    delete f;
}

auto RhiDeviceVulkan::destroyCommandBuffer(RhiCommandBuffer* cmd) -> void {
    auto* cb = static_cast<RhiCommandBufferVulkan*>(cmd);
    vkFreeCommandBuffers(device, cmdPool, 1, &cb->cmd);
    delete cb;
}
