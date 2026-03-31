#include "vulkan/vulkan_core.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

typedef struct {
    float m[4][4];
} mat4;

static mat4 mat4_identity(void) {
    mat4 r = {{}};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
    return r;
}

__attribute__((unused))
static mat4 mat4_mul(mat4 a, mat4 b) {
    mat4 r = {{}};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

static mat4 mat4_rotate_z(float angle) {
    mat4 r = mat4_identity();
    float c = cosf(angle), s = sinf(angle);
    r.m[0][0] = c;  r.m[0][1] = s;
    r.m[1][0] = -s; r.m[1][1] = c;
    return r;
}

static mat4 mat4_perspective(float fovY, float aspect, float near, float far) {
    float tanHalf = tanf(fovY / 2.0f);
    mat4 r = {{}};
    r.m[0][0] = 1.0f / (aspect * tanHalf);
    r.m[1][1] = 1.0f / tanHalf;
    r.m[2][2] = far / (near - far);
    r.m[2][3] = -1.0f;
    r.m[3][2] = (near * far) / (near - far);
    return r;
}

static mat4 mat4_lookAt(float eyeX, float eyeY, float eyeZ,
                        float centerX, float centerY, float centerZ,
                        float upX, float upY, float upZ) {
    float fx = centerX - eyeX, fy = centerY - eyeY, fz = centerZ - eyeZ;
    float len = sqrtf(fx*fx + fy*fy + fz*fz);
    fx /= len; fy /= len; fz /= len;

    float sx = fy*upZ - fz*upY, sy = fz*upX - fx*upZ, sz = fx*upY - fy*upX;
    len = sqrtf(sx*sx + sy*sy + sz*sz);
    sx /= len; sy /= len; sz /= len;

    float ux = sy*fz - sz*fy, uy = sz*fx - sx*fz, uz = sx*fy - sy*fx;

    mat4 r = mat4_identity();
    r.m[0][0] = sx;  r.m[1][0] = sx;  // will be overwritten below
    r.m[0][0] = sx; r.m[0][1] = ux; r.m[0][2] = -fx;
    r.m[1][0] = sy; r.m[1][1] = uy; r.m[1][2] = -fy;
    r.m[2][0] = sz; r.m[2][1] = uz; r.m[2][2] = -fz;
    r.m[3][0] = -(sx*eyeX + sy*eyeY + sz*eyeZ);
    r.m[3][1] = -(ux*eyeX + uy*eyeY + uz*eyeZ);
    r.m[3][2] =  (fx*eyeX + fy*eyeY + fz*eyeZ);
    return r;
}

typedef struct {
    mat4 model;
    mat4 view;
    mat4 proj;
} UniformBufferObject;

typedef struct {
    float position[2];
    float color[3];
} Vertex;

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
    Vertex vertices[] = {
        { .position = { -0.5f, -0.5f }, .color = { 1.0f, 0.0f, 0.0f } },
        { .position = {  0.5f, -0.5f }, .color = { 0.0f, 1.0f, 0.0f } },
        { .position = {  0.5f,  0.5f }, .color = { 0.0f, 0.0f, 1.0f } },
        { .position = { -0.5f,  0.5f }, .color = { 1.0f, 1.0f, 1.0f } },
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
    uint16_t indices[] = { 0, 1, 2, 2, 3, 0 };
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

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = 1280;
        extent.height = 720;
    }

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

    // 7. Create Render Pass
    VkAttachmentDescription colorAttachment = {
        .format = format.format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };

    VkAttachmentReference colorRef = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorRef,
    };

    VkRenderPassCreateInfo renderPassInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &colorAttachment,
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
        VkFramebufferCreateInfo framebufferInfo = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = renderPass,
            .attachmentCount = 1,
            .pAttachments = &imageViews[i],
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
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = offsetof(Vertex, position),
        },
        {
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(Vertex, color),
        },
    };

    VkPipelineVertexInputStateCreateInfo vertexInputState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bindingDesc,
        .vertexAttributeDescriptionCount = 2,
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
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisampleState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
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
    VkDescriptorSetLayoutBinding uboBinding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    };

    VkDescriptorSetLayout descriptorSetLayout;
    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &uboBinding,
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
    VkDescriptorPoolSize poolSize = {
        .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = imageCount,
    };
    VkDescriptorPool descriptorPool;
    VkDescriptorPoolCreateInfo descriptorPoolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = imageCount,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
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
        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptorSets[i],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &bufInfo,
        };
        vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
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
        float aspect = (float)extent.width / (float)extent.height;
        UniformBufferObject ubo = {
            .model = mat4_rotate_z(time),
            .view = mat4_lookAt(0.0f, 0.0f, 2.0f,  0.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f),
            .proj = mat4_perspective(45.0f * (3.14159265f / 180.0f), aspect, 0.1f, 10.0f),
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

        VkClearValue clear = {
            .color = { { 0.1f, 0.1f, 0.1f, 1.0f } },
        };
        VkRenderPassBeginInfo passBeginInfo = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = renderPass,
            .framebuffer = framebuffers[index],
            .renderArea = { { 0, 0 }, extent },
            .clearValueCount = 1,
            .pClearValues = &clear,
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

