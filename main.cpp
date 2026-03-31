#include "vulkan/vulkan_core.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

struct UniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
};

struct Vertex {
    float position[3];
    float normal[3];
    float color[3];
    float texCoord[2];
};

static uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
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

static int createBuffer(VkDevice device, VkPhysicalDevice physicalDevice, VkDeviceSize size,
                        VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                        VkBuffer* buffer, VkDeviceMemory* memory) {
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = vkCreateBuffer(device, &bufferInfo, NULL, buffer);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateBuffer failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, *buffer, &memReqs);

    uint32_t memTypeIndex = findMemoryType(physicalDevice, memReqs.memoryTypeBits, properties);
    if (memTypeIndex == UINT32_MAX) return 1;

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memTypeIndex,
    };
    result = vkAllocateMemory(device, &allocInfo, NULL, memory);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateMemory failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    result = vkBindBufferMemory(device, *buffer, *memory, 0);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkBindBufferMemory failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    return 0;
}

static int copyBuffer(VkDevice device, VkCommandPool cmdPool, VkQueue queue,
                      VkBuffer src, VkBuffer dst, VkDeviceSize size) {
    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    VkResult result = vkAllocateCommandBuffers(device, &allocInfo, &cmd);
    if (result != VK_SUCCESS) return 1;

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &beginInfo);
    VkBufferCopy region = { .size = size };
    vkCmdCopyBuffer(cmd, src, dst, 1, &region);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
    return 0;
}

static void transitionImageLayout(VkDevice device, VkCommandPool cmdPool, VkQueue queue,
                                  VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) {
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
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
}

VkShaderModule loadShaderModule(VkDevice device, const char* filepath) {
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        fprintf(stderr, "Failed to open shader filer: %s\n", filepath);
        return VK_NULL_HANDLE;
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

    VkShaderModule shaderModule;
    VkResult result = vkCreateShaderModule(device, &createInfo, NULL, &shaderModule);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateShaderModule failed: %s(%d)\n", string_VkResult(result), result);
        shaderModule = VK_NULL_HANDLE;
    }

    printf("Loaded shader: %s\n", filepath);
    free(code);

    return shaderModule;
}

// Steps to setup Vulkan
//
// 1. Initialize SDL and Create Vulkan Surface
// 2. Create Vulkan Instance
// 3. Select Physical Device and Queue Family
// 4. Create Logical Device and Graphics Queue
// 5. Create Swapchain
// 6. Create Image Views
// 7. Create Render Pass
// 8. Create Framebuffers
// 9. Create Shader Modules
// 10. Create Graphics Pipeline
// 11. Allocate Command Buffers
// 12. Create Sync Objects
// 13. Main loop
// 14. Clean up

int main(int argc, char* argv[]) {
    // 1. SDL init and window stuff.
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_PropertiesID windowProps = SDL_CreateProperties();
    SDL_SetStringProperty(windowProps, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "ngen");
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 1920);
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 1080);
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, 1);
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, 1);
    SDL_Window* window = SDL_CreateWindowWithProperties(windowProps);
    if (window == NULL) {
        fprintf(stderr, "SDL_CreateWindowWithProperties failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_DestroyProperties(windowProps);

    // 2. Create Vulkan instance and SDL surface.
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
        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR, // Needed for MoltenVk/macOS
#else
        .flags = 0,
#endif
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = validationLayersCount,
        .ppEnabledLayerNames = validationLayers,
        .enabledExtensionCount = extensionsCount,
        .ppEnabledExtensionNames = extensions,
    };

    VkInstance instance;
    result = vkCreateInstance(&instanceCreateInfo, NULL, &instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    VkSurfaceKHR surface;
    if (!SDL_Vulkan_CreateSurface(window, instance, NULL, &surface)) {
        fprintf(stderr, "SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return 1;
    }

    // 3. Physical device
    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkEnumeratePhysicalDevices failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkEnumeratePhysicalDevices failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = UINT32_MAX;

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


    // 4. Create logical device and graphic queue.
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

    VkDevice device;
    result = vkCreateDevice(physicalDevice, &deviceCreateInfo, NULL, &device);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDevice failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }
    
    VkQueue graphicsQueue;
    vkGetDeviceQueue(device, queueFamilyIndex, 0, &graphicsQueue);

    // Create Command Pool (needed for staging buffer transfers)
    VkCommandPool cmdPool;
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

    // Create Vertex Buffer (device-local via staging)
    // Cube: 24 vertices (4 per face for correct normals), 36 indices
    Vertex vertices[] = {
        // Front face (z = +0.5)
        { .position = { -0.5f, -0.5f,  0.5f }, .normal = { 0, 0, 1 }, .color = { 1, 1, 1 }, .texCoord = { 0, 0 } },
        { .position = {  0.5f, -0.5f,  0.5f }, .normal = { 0, 0, 1 }, .color = { 1, 1, 1 }, .texCoord = { 1, 0 } },
        { .position = {  0.5f,  0.5f,  0.5f }, .normal = { 0, 0, 1 }, .color = { 1, 1, 1 }, .texCoord = { 1, 1 } },
        { .position = { -0.5f,  0.5f,  0.5f }, .normal = { 0, 0, 1 }, .color = { 1, 1, 1 }, .texCoord = { 0, 1 } },
        // Back face (z = -0.5)
        { .position = {  0.5f, -0.5f, -0.5f }, .normal = { 0, 0, -1 }, .color = { 1, 1, 1 }, .texCoord = { 0, 0 } },
        { .position = { -0.5f, -0.5f, -0.5f }, .normal = { 0, 0, -1 }, .color = { 1, 1, 1 }, .texCoord = { 1, 0 } },
        { .position = { -0.5f,  0.5f, -0.5f }, .normal = { 0, 0, -1 }, .color = { 1, 1, 1 }, .texCoord = { 1, 1 } },
        { .position = {  0.5f,  0.5f, -0.5f }, .normal = { 0, 0, -1 }, .color = { 1, 1, 1 }, .texCoord = { 0, 1 } },
        // Right face (x = +0.5)
        { .position = {  0.5f, -0.5f,  0.5f }, .normal = { 1, 0, 0 }, .color = { 1, 1, 1 }, .texCoord = { 0, 0 } },
        { .position = {  0.5f, -0.5f, -0.5f }, .normal = { 1, 0, 0 }, .color = { 1, 1, 1 }, .texCoord = { 1, 0 } },
        { .position = {  0.5f,  0.5f, -0.5f }, .normal = { 1, 0, 0 }, .color = { 1, 1, 1 }, .texCoord = { 1, 1 } },
        { .position = {  0.5f,  0.5f,  0.5f }, .normal = { 1, 0, 0 }, .color = { 1, 1, 1 }, .texCoord = { 0, 1 } },
        // Left face (x = -0.5)
        { .position = { -0.5f, -0.5f, -0.5f }, .normal = { -1, 0, 0 }, .color = { 1, 1, 1 }, .texCoord = { 0, 0 } },
        { .position = { -0.5f, -0.5f,  0.5f }, .normal = { -1, 0, 0 }, .color = { 1, 1, 1 }, .texCoord = { 1, 0 } },
        { .position = { -0.5f,  0.5f,  0.5f }, .normal = { -1, 0, 0 }, .color = { 1, 1, 1 }, .texCoord = { 1, 1 } },
        { .position = { -0.5f,  0.5f, -0.5f }, .normal = { -1, 0, 0 }, .color = { 1, 1, 1 }, .texCoord = { 0, 1 } },
        // Top face (y = +0.5)
        { .position = { -0.5f,  0.5f,  0.5f }, .normal = { 0, 1, 0 }, .color = { 1, 1, 1 }, .texCoord = { 0, 0 } },
        { .position = {  0.5f,  0.5f,  0.5f }, .normal = { 0, 1, 0 }, .color = { 1, 1, 1 }, .texCoord = { 1, 0 } },
        { .position = {  0.5f,  0.5f, -0.5f }, .normal = { 0, 1, 0 }, .color = { 1, 1, 1 }, .texCoord = { 1, 1 } },
        { .position = { -0.5f,  0.5f, -0.5f }, .normal = { 0, 1, 0 }, .color = { 1, 1, 1 }, .texCoord = { 0, 1 } },
        // Bottom face (y = -0.5)
        { .position = { -0.5f, -0.5f, -0.5f }, .normal = { 0, -1, 0 }, .color = { 1, 1, 1 }, .texCoord = { 0, 0 } },
        { .position = {  0.5f, -0.5f, -0.5f }, .normal = { 0, -1, 0 }, .color = { 1, 1, 1 }, .texCoord = { 1, 0 } },
        { .position = {  0.5f, -0.5f,  0.5f }, .normal = { 0, -1, 0 }, .color = { 1, 1, 1 }, .texCoord = { 1, 1 } },
        { .position = { -0.5f, -0.5f,  0.5f }, .normal = { 0, -1, 0 }, .color = { 1, 1, 1 }, .texCoord = { 0, 1 } },
    };
    VkDeviceSize vertexBufferSize = sizeof(vertices);

    VkBuffer vertexStaging;
    VkDeviceMemory vertexStagingMemory;
    if (createBuffer(device, physicalDevice, vertexBufferSize,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &vertexStaging, &vertexStagingMemory)) return 1;

    void* data;
    vkMapMemory(device, vertexStagingMemory, 0, vertexBufferSize, 0, &data);
    memcpy(data, vertices, vertexBufferSize);
    vkUnmapMemory(device, vertexStagingMemory);

    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;
    if (createBuffer(device, physicalDevice, vertexBufferSize,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     &vertexBuffer, &vertexBufferMemory)) return 1;

    copyBuffer(device, cmdPool, graphicsQueue, vertexStaging, vertexBuffer, vertexBufferSize);
    vkDestroyBuffer(device, vertexStaging, NULL);
    vkFreeMemory(device, vertexStagingMemory, NULL);

    // Create Index Buffer (device-local via staging)
    uint16_t indices[] = {
         0,  1,  2,  2,  3,  0, // front
         4,  5,  6,  6,  7,  4, // back
         8,  9, 10, 10, 11,  8, // right
        12, 13, 14, 14, 15, 12, // left
        16, 17, 18, 18, 19, 16, // top
        20, 21, 22, 22, 23, 20, // bottom
    };
    uint32_t indexCount = sizeof(indices) / sizeof(indices[0]);
    VkDeviceSize indexBufferSize = sizeof(indices);

    VkBuffer indexStaging;
    VkDeviceMemory indexStagingMemory;
    if (createBuffer(device, physicalDevice, indexBufferSize,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &indexStaging, &indexStagingMemory)) return 1;

    void* indexData;
    vkMapMemory(device, indexStagingMemory, 0, indexBufferSize, 0, &indexData);
    memcpy(indexData, indices, indexBufferSize);
    vkUnmapMemory(device, indexStagingMemory);

    VkBuffer indexBuffer;
    VkDeviceMemory indexBufferMemory;
    if (createBuffer(device, physicalDevice, indexBufferSize,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     &indexBuffer, &indexBufferMemory)) return 1;

    copyBuffer(device, cmdPool, graphicsQueue, indexStaging, indexBuffer, indexBufferSize);
    vkDestroyBuffer(device, indexStaging, NULL);
    vkFreeMemory(device, indexStagingMemory, NULL);

    // Create Texture (checkerboard pattern)
    const uint32_t texWidth = 64, texHeight = 64;
    const uint32_t texSize = texWidth * texHeight * 4;
    uint8_t texPixels[texWidth * texHeight * 4];
    for (uint32_t y = 0; y < texHeight; y++) {
        for (uint32_t x = 0; x < texWidth; x++) {
            uint8_t c = ((x / 8) + (y / 8)) % 2 ? 255 : 64;
            uint32_t i = (y * texWidth + x) * 4;
            texPixels[i + 0] = c;
            texPixels[i + 1] = c;
            texPixels[i + 2] = c;
            texPixels[i + 3] = 255;
        }
    }

    VkBuffer texStaging;
    VkDeviceMemory texStagingMemory;
    if (createBuffer(device, physicalDevice, texSize,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &texStaging, &texStagingMemory)) return 1;
    void* texData;
    vkMapMemory(device, texStagingMemory, 0, texSize, 0, &texData);
    memcpy(texData, texPixels, texSize);
    vkUnmapMemory(device, texStagingMemory);

    VkImage textureImage;
    VkDeviceMemory textureImageMemory;
    VkImageCreateInfo texImageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .extent = { texWidth, texHeight, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    result = vkCreateImage(device, &texImageInfo, NULL, &textureImage);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateImage failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    VkMemoryRequirements texMemReqs;
    vkGetImageMemoryRequirements(device, textureImage, &texMemReqs);
    uint32_t texMemType = findMemoryType(physicalDevice, texMemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (texMemType == UINT32_MAX) return 1;

    VkMemoryAllocateInfo texAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = texMemReqs.size,
        .memoryTypeIndex = texMemType,
    };
    result = vkAllocateMemory(device, &texAllocInfo, NULL, &textureImageMemory);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateMemory failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }
    vkBindImageMemory(device, textureImage, textureImageMemory, 0);

    // Upload: UNDEFINED -> TRANSFER_DST, copy, TRANSFER_DST -> SHADER_READ_ONLY
    transitionImageLayout(device, cmdPool, graphicsQueue, textureImage,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    {
        VkCommandBufferAllocateInfo cmdAllocInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = cmdPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmd);
        VkCommandBufferBeginInfo beginInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(cmd, &beginInfo);
        VkBufferImageCopy copyRegion = {
            .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
            .imageExtent = { texWidth, texHeight, 1 },
        };
        vkCmdCopyBufferToImage(cmd, texStaging, textureImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
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

    transitionImageLayout(device, cmdPool, graphicsQueue, textureImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(device, texStaging, NULL);
    vkFreeMemory(device, texStagingMemory, NULL);

    // Texture Image View
    VkImageView textureImageView;
    VkImageViewCreateInfo texViewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = textureImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    result = vkCreateImageView(device, &texViewInfo, NULL, &textureImageView);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateImageView failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    // Texture Sampler
    VkSampler textureSampler;
    VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
    };
    result = vkCreateSampler(device, &samplerInfo, NULL, &textureSampler);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateSampler failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    // 5. Create Swapchain
    VkSurfaceCapabilitiesKHR capabilities;
    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    uint32_t formatCount;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    VkSurfaceFormatKHR format = formats[0];

    uint32_t presentModeCount;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkGetPhysicalDeviceSurfacePresentModesKHR failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, presentModes.data());
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkGetPhysicalDeviceSurfacePresentModesKHR failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

    VkExtent2D extent;
    {
        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        extent.width = (uint32_t)w;
        extent.height = (uint32_t)h;
    }
    // Clamp to surface capabilities
    if (extent.width < capabilities.minImageExtent.width) extent.width = capabilities.minImageExtent.width;
    if (extent.width > capabilities.maxImageExtent.width) extent.width = capabilities.maxImageExtent.width;
    if (extent.height < capabilities.minImageExtent.height) extent.height = capabilities.minImageExtent.height;
    if (extent.height > capabilities.maxImageExtent.height) extent.height = capabilities.maxImageExtent.height;
    printf("Swapchain extent: %dx%d\n", extent.width, extent.height);

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swapchainInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = imageCount,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = presentMode,
        .clipped = VK_TRUE,
    };

    VkSwapchainKHR swapchain;
    result = vkCreateSwapchainKHR(device, &swapchainInfo, NULL, &swapchain);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateSwapchainKHR failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    std::vector<VkImage> images(imageCount);
    result = vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());

    // 6. Create Image Views
    std::vector<VkImageView> imageViews(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format.format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        
        result = vkCreateImageView(device, &viewInfo, NULL, &imageViews[i]);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkCreateImageView failed: %s(%d)\n", string_VkResult(result), result);
            return 1;
        }
    }

    // Create Depth Image
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    VkImageCreateInfo depthImageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = depthFormat,
        .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage depthImage;
    result = vkCreateImage(device, &depthImageInfo, NULL, &depthImage);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateImage failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    VkMemoryRequirements depthMemReqs;
    vkGetImageMemoryRequirements(device, depthImage, &depthMemReqs);

    uint32_t depthMemType = findMemoryType(physicalDevice, depthMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (depthMemType == UINT32_MAX) return 1;

    VkDeviceMemory depthImageMemory;
    VkMemoryAllocateInfo depthAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = depthMemReqs.size,
        .memoryTypeIndex = depthMemType,
    };
    result = vkAllocateMemory(device, &depthAllocInfo, NULL, &depthImageMemory);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateMemory failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }
    vkBindImageMemory(device, depthImage, depthImageMemory, 0);

    VkImageView depthImageView;
    VkImageViewCreateInfo depthViewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = depthImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = depthFormat,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    result = vkCreateImageView(device, &depthViewInfo, NULL, &depthImageView);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateImageView failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    // 7. Create Render Pass
    VkAttachmentDescription attachments[] = {
        {
            .format = format.format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        },
        {
            .format = depthFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
    };

    VkAttachmentReference colorRef = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentReference depthRef = {
        .attachment = 1,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorRef,
        .pDepthStencilAttachment = &depthRef,
    };

    VkRenderPassCreateInfo renderPassInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
    };

    VkRenderPass renderPass;
    result = vkCreateRenderPass(device, &renderPassInfo, NULL, &renderPass);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateRenderPass failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    // 8. Create Framebuffers
    std::vector<VkFramebuffer> framebuffers(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageView fbAttachments[] = { imageViews[i], depthImageView };
        VkFramebufferCreateInfo framebufferInfo = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = renderPass,
            .attachmentCount = 2,
            .pAttachments = fbAttachments,
            .width = extent.width,
            .height = extent.height,
            .layers = 1,
        };

        result = vkCreateFramebuffer(device, &framebufferInfo, NULL, &framebuffers[i]);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkCreateFramebuffer failed: %s(%d)\n", string_VkResult(result), result);
            return 1;
        }
    }

    // 9. Create Shader Modules
    VkShaderModule vertShader = loadShaderModule(device, "shaders/triangle.vert.spv");
    VkShaderModule fragShader = loadShaderModule(device, "shaders/triangle.frag.spv");

    // 10. Create Graphics Pipeline
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
        {
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(Vertex, position),
        },
        {
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(Vertex, normal),
        },
        {
            .location = 2,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(Vertex, color),
        },
        {
            .location = 3,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = offsetof(Vertex, texCoord),
        },
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

    VkViewport viewport = { 0, 0, (float)extent.width, (float)extent.height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, extent };
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

    // Descriptor Set Layout
    VkDescriptorSetLayoutBinding layoutBindings[] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };

    VkDescriptorSetLayout descriptorSetLayout;
    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = layoutBindings,
    };
    result = vkCreateDescriptorSetLayout(device, &descriptorSetLayoutInfo, NULL, &descriptorSetLayout);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDescriptorSetLayout failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    VkPipelineLayout layout;
    VkPipelineLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptorSetLayout,
    };

    result = vkCreatePipelineLayout(device, &layoutInfo, NULL, &layout);
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
        .layout = layout,
        .renderPass = renderPass,
    };

    VkPipeline pipeline;
    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsPipelineInfo, NULL, &pipeline);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateGraphicsPipelines failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    // Create Uniform Buffers (one per swapchain image)
    std::vector<VkBuffer> uniformBuffers(imageCount);
    std::vector<VkDeviceMemory> uniformBuffersMemory(imageCount);
    std::vector<void*> uniformBuffersMapped(imageCount);

    for (uint32_t i = 0; i < imageCount; i++) {
        if (createBuffer(device, physicalDevice, sizeof(UniformBufferObject),
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &uniformBuffers[i], &uniformBuffersMemory[i])) return 1;
        vkMapMemory(device, uniformBuffersMemory[i], 0, sizeof(UniformBufferObject), 0, &uniformBuffersMapped[i]);
    }

    // Descriptor Pool and Sets
    VkDescriptorPoolSize poolSizes[] = {
        { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = imageCount },
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = imageCount },
    };
    VkDescriptorPool descriptorPool;
    VkDescriptorPoolCreateInfo descriptorPoolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = imageCount,
        .poolSizeCount = 2,
        .pPoolSizes = poolSizes,
    };
    result = vkCreateDescriptorPool(device, &descriptorPoolInfo, NULL, &descriptorPool);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDescriptorPool failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    std::vector<VkDescriptorSetLayout> layouts(imageCount, descriptorSetLayout);

    std::vector<VkDescriptorSet> descriptorSets(imageCount);
    VkDescriptorSetAllocateInfo descriptorAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = imageCount,
        .pSetLayouts = layouts.data(),
    };
    result = vkAllocateDescriptorSets(device, &descriptorAllocInfo, descriptorSets.data());
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateDescriptorSets failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    for (uint32_t i = 0; i < imageCount; i++) {
        VkDescriptorBufferInfo bufInfo = {
            .buffer = uniformBuffers[i],
            .offset = 0,
            .range = sizeof(UniformBufferObject),
        };
        VkDescriptorImageInfo imgInfo = {
            .sampler = textureSampler,
            .imageView = textureImageView,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet writes[] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = descriptorSets[i],
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pBufferInfo = &bufInfo,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = descriptorSets[i],
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &imgInfo,
            },
        };
        vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    }

    // 11. Allocate Command Buffers
    std::vector<VkCommandBuffer> cmdBuffers(imageCount);
    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = imageCount,
    };
    result = vkAllocateCommandBuffers(device, &allocInfo, cmdBuffers.data());
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateCommandBuffers failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    // Command buffers are now recorded per-frame in the main loop.

    // 12. Create Sync Objects (per swapchain image)
    std::vector<VkSemaphore> imageAvailableSemaphores(imageCount);
    std::vector<VkSemaphore> renderFinishedSemaphores(imageCount);
    std::vector<VkFence> inflightFences(imageCount);

    VkSemaphoreCreateInfo semInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkFenceCreateInfo fenceInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    for (uint32_t i = 0; i < imageCount; i++) {
        result = vkCreateSemaphore(device, &semInfo, NULL, &imageAvailableSemaphores[i]);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkCreateSemaphore failed: %s(%d)\n", string_VkResult(result), result);
            return 1;
        }

        result = vkCreateSemaphore(device, &semInfo, NULL, &renderFinishedSemaphores[i]);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkCreateSemaphore failed: %s(%d)\n", string_VkResult(result), result);
            return 1;
        }

        result = vkCreateFence(device, &fenceInfo, NULL, &inflightFences[i]);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkCreateFence failed: %s(%d)\n", string_VkResult(result), result);
            return 1;
        }
    }

    uint32_t currentFrame = 0;

    // 13. Main loop
    uint64_t startTicks = SDL_GetTicksNS();
    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                printf("Quitting\n");
                quit = true;
            }

            if (ev.type == SDL_EVENT_WINDOW_RESIZED) {
                int newWidth;
                int newHeight;
                if (!SDL_GetWindowSizeInPixels(window, &newWidth, &newHeight)) {
                    fprintf(stderr, "SDL_GetWindowSizeInPixels failed: %s\n", SDL_GetError());
                    return 1;
                }

                printf("Window resized to %dx%d\n", newWidth, newHeight);

                result = vkDeviceWaitIdle(device);
                if (result != VK_SUCCESS) {
                    fprintf(stderr, "vkDeviceWaitIdle failed: %s(%d)\n", string_VkResult(result), result);
                    return 1;
                }
            }
        }

        result = vkWaitForFences(device, 1, &inflightFences[currentFrame], VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkWaitForFences failed: %s(%d)\n", string_VkResult(result), result);
            return 1;
        }

        uint32_t index;
        result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &index);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkAcquireNextImageKHR failed: %s(%d)\n", string_VkResult(result), result);
            return 1;
        }

        result = vkResetFences(device, 1, &inflightFences[currentFrame]);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkResetFences failed: %s(%d)\n", string_VkResult(result), result);
            return 1;
        }

        // Update uniform buffer
        float time = (float)(SDL_GetTicksNS() - startTicks) / 1.0e9f;
        int winW, winH;
        SDL_GetWindowSizeInPixels(window, &winW, &winH);
        float aspect = (float)winW / (float)winH;
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 10.0f);
        proj[1][1] *= -1.0f; // Flip Y for Vulkan clip coordinates
        UniformBufferObject ubo = {
            .model = glm::rotate(glm::mat4(1.0f), time, glm::vec3(0.0f, 1.0f, 0.0f)),
            .view = glm::lookAt(glm::vec3(2.0f, 1.5f, 2.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
            .proj = proj,
        };
        memcpy(uniformBuffersMapped[index], &ubo, sizeof(ubo));

        // Record command buffer for this frame
        vkResetCommandBuffer(cmdBuffers[index], 0);

        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        result = vkBeginCommandBuffer(cmdBuffers[index], &begin);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkBeginCommandBuffer failed: %s(%d)\n", string_VkResult(result), result);
            return 1;
        }

        VkClearValue clearValues[2] = {};
        clearValues[0].color = { { 0.1f, 0.1f, 0.1f, 1.0f } };
        clearValues[1].depthStencil = { 1.0f, 0 };

        VkRenderPassBeginInfo passBeginInfo = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = renderPass,
            .framebuffer = framebuffers[index],
            .renderArea = { { 0, 0 }, extent },
            .clearValueCount = 2,
            .pClearValues = clearValues,
        };

        vkCmdBeginRenderPass(cmdBuffers[index], &passBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmdBuffers[index], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(cmdBuffers[index], 0, 1, &vertexBuffer, offsets);
        vkCmdBindIndexBuffer(cmdBuffers[index], indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdBindDescriptorSets(cmdBuffers[index], VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descriptorSets[index], 0, NULL);
        vkCmdDrawIndexed(cmdBuffers[index], indexCount, 1, 0, 0, 0);
        vkCmdEndRenderPass(cmdBuffers[index]);
        result = vkEndCommandBuffer(cmdBuffers[index]);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkEndCommandBuffer failed: %s(%d)\n", string_VkResult(result), result);
            return 1;
        }

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

        result = vkQueueSubmit(graphicsQueue, 1, &submit, inflightFences[currentFrame]);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkQueueSubmit failed: %s(%d)\n", string_VkResult(result), result);
            return 1;
        }

        VkPresentInfoKHR present = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &renderFinishedSemaphores[currentFrame],
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &index,
        };

        result = vkQueuePresentKHR(graphicsQueue, &present);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkQueuePresentKHR failed: %s(%d)\n", string_VkResult(result), result);
            return 1;
        }

        currentFrame = (currentFrame + 1) % imageCount;
    }

    // 14. Clean up.
    result = vkDeviceWaitIdle(device);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkDeviceWaitIdle failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    vkDestroyImageView(device, depthImageView, NULL);
    vkDestroyImage(device, depthImage, NULL);
    vkFreeMemory(device, depthImageMemory, NULL);

    for (uint32_t i = 0; i < imageCount; i++) {
        vkDestroyFramebuffer(device, framebuffers[i], NULL);
        vkDestroyImageView(device, imageViews[i], NULL);
    }

    vkDestroyDescriptorPool(device, descriptorPool, NULL);
    vkDestroyDescriptorSetLayout(device, descriptorSetLayout, NULL);
    for (uint32_t i = 0; i < imageCount; i++) {
        vkUnmapMemory(device, uniformBuffersMemory[i]);
        vkDestroyBuffer(device, uniformBuffers[i], NULL);
        vkFreeMemory(device, uniformBuffersMemory[i], NULL);
    }

    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyRenderPass(device, renderPass, NULL);

    vkDestroyShaderModule(device, vertShader, NULL);
    vkDestroyShaderModule(device, fragShader, NULL);

    vkDestroySwapchainKHR(device, swapchain, NULL);

    vkDestroySampler(device, textureSampler, NULL);
    vkDestroyImageView(device, textureImageView, NULL);
    vkDestroyImage(device, textureImage, NULL);
    vkFreeMemory(device, textureImageMemory, NULL);

    vkDestroyBuffer(device, vertexBuffer, NULL);
    vkFreeMemory(device, vertexBufferMemory, NULL);
    vkDestroyBuffer(device, indexBuffer, NULL);
    vkFreeMemory(device, indexBufferMemory, NULL);

    for (uint32_t i = 0; i < imageCount; i++) {
        vkDestroySemaphore(device, imageAvailableSemaphores[i], NULL);
        vkDestroySemaphore(device, renderFinishedSemaphores[i], NULL);
        vkDestroyFence(device, inflightFences[i], NULL);
    }

    vkDestroyCommandPool(device, cmdPool, NULL);

    vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyInstance(instance, NULL);

    SDL_DestroyWindow(window); 
    SDL_Quit();

    return 0;
}

