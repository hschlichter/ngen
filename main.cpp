#include "vulkan/vulkan_core.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

struct UniformBufferObject {
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

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    int texWidth = 0, texHeight = 0;
    std::vector<uint8_t> texPixels; // RGBA
    glm::mat4 transform = glm::mat4(1.0f);
};

// GPU-side resources for a single mesh, created after Vulkan init
struct GpuMesh {
    VkBuffer vertexBuffer, indexBuffer;
    VkDeviceMemory vertexMemory, indexMemory;
    uint32_t indexCount;
    VkImage textureImage;
    VkDeviceMemory textureMemory;
    VkImageView textureView;
    glm::mat4 transform;
};

struct Scene {
    std::vector<MeshData> meshes;
};

static void loadPrimitive(MeshData& mesh, cgltf_primitive* prim, const char* filepath) {
    if (prim->indices) {
        size_t count = prim->indices->count;
        mesh.indices.resize(count);
        for (size_t i = 0; i < count; i++)
            mesh.indices[i] = (uint32_t)cgltf_accessor_read_index(prim->indices, i);
    }

    cgltf_accessor* posAccessor = nullptr;
    cgltf_accessor* normAccessor = nullptr;
    cgltf_accessor* uvAccessor = nullptr;

    for (size_t i = 0; i < prim->attributes_count; i++) {
        if (prim->attributes[i].type == cgltf_attribute_type_position)
            posAccessor = prim->attributes[i].data;
        else if (prim->attributes[i].type == cgltf_attribute_type_normal)
            normAccessor = prim->attributes[i].data;
        else if (prim->attributes[i].type == cgltf_attribute_type_texcoord)
            uvAccessor = prim->attributes[i].data;
    }

    if (!posAccessor) return;
    size_t vertCount = posAccessor->count;
    mesh.vertices.resize(vertCount);

    for (size_t i = 0; i < vertCount; i++) {
        Vertex& v = mesh.vertices[i];
        cgltf_accessor_read_float(posAccessor, i, v.position, 3);
        if (normAccessor) cgltf_accessor_read_float(normAccessor, i, v.normal, 3);
        else { v.normal[0] = v.normal[1] = 0; v.normal[2] = 1; }
        if (uvAccessor) cgltf_accessor_read_float(uvAccessor, i, v.texCoord, 2);
        else { v.texCoord[0] = v.texCoord[1] = 0; }
        v.color[0] = v.color[1] = v.color[2] = 1.0f;
    }

    if (mesh.indices.empty()) {
        mesh.indices.resize(vertCount);
        for (size_t i = 0; i < vertCount; i++) mesh.indices[i] = (uint32_t)i;
    }

    if (prim->material && prim->material->pbr_metallic_roughness.base_color_texture.texture) {
        cgltf_image* img = prim->material->pbr_metallic_roughness.base_color_texture.texture->image;
        if (img) {
            int channels;
            uint8_t* pixels = nullptr;
            if (img->buffer_view) {
                const uint8_t* bufData = (const uint8_t*)img->buffer_view->buffer->data + img->buffer_view->offset;
                pixels = stbi_load_from_memory(bufData, (int)img->buffer_view->size,
                    &mesh.texWidth, &mesh.texHeight, &channels, 4);
            } else if (img->uri) {
                std::string dir(filepath);
                size_t slash = dir.find_last_of("/\\");
                std::string texPath = (slash != std::string::npos ? dir.substr(0, slash + 1) : "") + img->uri;
                pixels = stbi_load(texPath.c_str(), &mesh.texWidth, &mesh.texHeight, &channels, 4);
            }
            if (pixels) {
                mesh.texPixels.assign(pixels, pixels + (size_t)mesh.texWidth * mesh.texHeight * 4);
                stbi_image_free(pixels);
            }
        }
    }
}

static glm::mat4 getNodeTransform(cgltf_node* node) {
    glm::mat4 t(1.0f);
    if (node->has_matrix) {
        memcpy(&t, node->matrix, sizeof(float) * 16);
    } else {
        if (node->has_translation)
            t = glm::translate(t, glm::vec3(node->translation[0], node->translation[1], node->translation[2]));
        if (node->has_rotation) {
            glm::quat q(node->rotation[3], node->rotation[0], node->rotation[1], node->rotation[2]);
            t *= glm::mat4_cast(q);
        }
        if (node->has_scale)
            t = glm::scale(t, glm::vec3(node->scale[0], node->scale[1], node->scale[2]));
    }
    return t;
}

static void processNode(Scene& scene, cgltf_node* node, const glm::mat4& parentTransform, const char* filepath) {
    glm::mat4 world = parentTransform * getNodeTransform(node);
    if (node->mesh) {
        for (size_t p = 0; p < node->mesh->primitives_count; p++) {
            MeshData md;
            md.transform = world;
            loadPrimitive(md, &node->mesh->primitives[p], filepath);
            if (!md.vertices.empty()) {
                printf("  mesh: %zu verts, %zu indices, tex %dx%d\n",
                    md.vertices.size(), md.indices.size(), md.texWidth, md.texHeight);
                scene.meshes.push_back(std::move(md));
            }
        }
    }
    for (size_t i = 0; i < node->children_count; i++)
        processNode(scene, node->children[i], world, filepath);
}

static Scene loadGltf(const char* filepath) {
    Scene scene;
    cgltf_options options = {};
    cgltf_data* gltf = nullptr;
    if (cgltf_parse_file(&options, filepath, &gltf) != cgltf_result_success) {
        fprintf(stderr, "cgltf_parse_file failed\n");
        return scene;
    }
    if (cgltf_load_buffers(&options, gltf, filepath) != cgltf_result_success) {
        fprintf(stderr, "cgltf_load_buffers failed\n");
        cgltf_free(gltf);
        return scene;
    }

    if (gltf->scene) {
        for (size_t i = 0; i < gltf->scene->nodes_count; i++)
            processNode(scene, gltf->scene->nodes[i], glm::mat4(1.0f), filepath);
    } else {
        for (size_t i = 0; i < gltf->nodes_count; i++)
            if (!gltf->nodes[i].parent)
                processNode(scene, &gltf->nodes[i], glm::mat4(1.0f), filepath);
    }

    printf("Loaded %zu mesh(es)\n", scene.meshes.size());
    cgltf_free(gltf);
    return scene;
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
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.gltf>\n", argv[0]);
        return 1;
    }

    Scene scene = loadGltf(argv[1]);
    if (scene.meshes.empty()) {
        fprintf(stderr, "Failed to load model\n");
        return 1;
    }

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

    // Create GPU resources for each mesh
    std::vector<GpuMesh> gpuMeshes(scene.meshes.size());
    // Shared sampler
    VkSampler textureSampler;
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
        result = vkCreateSampler(device, &samplerInfo, NULL, &textureSampler);
        if (result != VK_SUCCESS) { fprintf(stderr, "vkCreateSampler failed\n"); return 1; }
    }

    // Fallback checkerboard texture
    std::vector<uint8_t> fallbackPixels(64 * 64 * 4);
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            uint8_t c = ((x / 8) + (y / 8)) % 2 ? 255 : 64;
            uint32_t i = (y * 64 + x) * 4;
            fallbackPixels[i] = fallbackPixels[i+1] = fallbackPixels[i+2] = c;
            fallbackPixels[i+3] = 255;
        }

    for (size_t m = 0; m < scene.meshes.size(); m++) {
        MeshData& md = scene.meshes[m];
        GpuMesh& gm = gpuMeshes[m];
        gm.transform = md.transform;
        gm.indexCount = (uint32_t)md.indices.size();

        // Vertex buffer
        VkDeviceSize vbSize = md.vertices.size() * sizeof(Vertex);
        VkBuffer vStaging; VkDeviceMemory vStagingMem;
        createBuffer(device, physicalDevice, vbSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vStaging, &vStagingMem);
        void* data;
        vkMapMemory(device, vStagingMem, 0, vbSize, 0, &data);
        memcpy(data, md.vertices.data(), vbSize);
        vkUnmapMemory(device, vStagingMem);
        createBuffer(device, physicalDevice, vbSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &gm.vertexBuffer, &gm.vertexMemory);
        copyBuffer(device, cmdPool, graphicsQueue, vStaging, gm.vertexBuffer, vbSize);
        vkDestroyBuffer(device, vStaging, NULL); vkFreeMemory(device, vStagingMem, NULL);

        // Index buffer
        VkDeviceSize ibSize = md.indices.size() * sizeof(uint32_t);
        VkBuffer iStaging; VkDeviceMemory iStagingMem;
        createBuffer(device, physicalDevice, ibSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &iStaging, &iStagingMem);
        vkMapMemory(device, iStagingMem, 0, ibSize, 0, &data);
        memcpy(data, md.indices.data(), ibSize);
        vkUnmapMemory(device, iStagingMem);
        createBuffer(device, physicalDevice, ibSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &gm.indexBuffer, &gm.indexMemory);
        copyBuffer(device, cmdPool, graphicsQueue, iStaging, gm.indexBuffer, ibSize);
        vkDestroyBuffer(device, iStaging, NULL); vkFreeMemory(device, iStagingMem, NULL);

        // Texture
        uint32_t tw, th; const uint8_t* texPtr;
        if (!md.texPixels.empty()) { tw = md.texWidth; th = md.texHeight; texPtr = md.texPixels.data(); }
        else { tw = 64; th = 64; texPtr = fallbackPixels.data(); }
        uint32_t texSize = tw * th * 4;

        VkBuffer tStaging; VkDeviceMemory tStagingMem;
        createBuffer(device, physicalDevice, texSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &tStaging, &tStagingMem);
        vkMapMemory(device, tStagingMem, 0, texSize, 0, &data);
        memcpy(data, texPtr, texSize);
        vkUnmapMemory(device, tStagingMem);

        VkImageCreateInfo texImageInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_SRGB, .extent = { tw, th, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        vkCreateImage(device, &texImageInfo, NULL, &gm.textureImage);
        VkMemoryRequirements texMemReqs;
        vkGetImageMemoryRequirements(device, gm.textureImage, &texMemReqs);
        VkMemoryAllocateInfo texAllocInfo = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = texMemReqs.size,
            .memoryTypeIndex = findMemoryType(physicalDevice, texMemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };
        vkAllocateMemory(device, &texAllocInfo, NULL, &gm.textureMemory);
        vkBindImageMemory(device, gm.textureImage, gm.textureMemory, 0);

        transitionImageLayout(device, cmdPool, graphicsQueue, gm.textureImage,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        {
            VkCommandBufferAllocateInfo ca = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = cmdPool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
            VkCommandBuffer cmd; vkAllocateCommandBuffers(device, &ca, &cmd);
            VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
            vkBeginCommandBuffer(cmd, &bi);
            VkBufferImageCopy region = { .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
                .imageExtent = { tw, th, 1 } };
            vkCmdCopyBufferToImage(cmd, tStaging, gm.textureImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
            vkQueueSubmit(graphicsQueue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(graphicsQueue);
            vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
        }
        transitionImageLayout(device, cmdPool, graphicsQueue, gm.textureImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkDestroyBuffer(device, tStaging, NULL); vkFreeMemory(device, tStagingMem, NULL);

        VkImageViewCreateInfo tvInfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = gm.textureImage, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_SRGB,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 } };
        vkCreateImageView(device, &tvInfo, NULL, &gm.textureView);
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

    VkPushConstantRange pushConstRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(glm::mat4),
    };

    VkPipelineLayout layout;
    VkPipelineLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptorSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstRange,
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

    // Descriptor Pool and Sets — one set per frame per mesh
    uint32_t meshCount = (uint32_t)gpuMeshes.size();
    uint32_t totalSets = imageCount * meshCount;
    VkDescriptorPoolSize poolSizes[] = {
        { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = totalSets },
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = totalSets },
    };
    VkDescriptorPool descriptorPool;
    VkDescriptorPoolCreateInfo descriptorPoolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = totalSets,
        .poolSizeCount = 2,
        .pPoolSizes = poolSizes,
    };
    result = vkCreateDescriptorPool(device, &descriptorPoolInfo, NULL, &descriptorPool);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDescriptorPool failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    // descriptorSets[frame * meshCount + meshIdx]
    std::vector<VkDescriptorSetLayout> dsLayouts(totalSets, descriptorSetLayout);
    std::vector<VkDescriptorSet> descriptorSets(totalSets);
    VkDescriptorSetAllocateInfo descriptorAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = totalSets,
        .pSetLayouts = dsLayouts.data(),
    };
    result = vkAllocateDescriptorSets(device, &descriptorAllocInfo, descriptorSets.data());
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateDescriptorSets failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    for (uint32_t i = 0; i < imageCount; i++) {
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
            vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
        }
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

        // Update uniform buffer (view + proj only, model is per-mesh via push constants)
        float time = (float)(SDL_GetTicksNS() - startTicks) / 1.0e9f;
        int winW, winH;
        SDL_GetWindowSizeInPixels(window, &winW, &winH);
        float aspect = (float)winW / (float)winH;
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 10.0f);
        proj[1][1] *= -1.0f; // Flip Y for Vulkan clip coordinates
        UniformBufferObject ubo = {
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

        glm::mat4 rootModel = glm::rotate(glm::mat4(1.0f), time, glm::vec3(0.0f, 1.0f, 0.0f));
        for (uint32_t m = 0; m < meshCount; m++) {
            GpuMesh& gm = gpuMeshes[m];
            glm::mat4 model = rootModel * gm.transform;
            vkCmdPushConstants(cmdBuffers[index], layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &model);
            VkDeviceSize offsets[] = { 0 };
            vkCmdBindVertexBuffers(cmdBuffers[index], 0, 1, &gm.vertexBuffer, offsets);
            vkCmdBindIndexBuffer(cmdBuffers[index], gm.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdBindDescriptorSets(cmdBuffers[index], VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                &descriptorSets[index * meshCount + m], 0, NULL);
            vkCmdDrawIndexed(cmdBuffers[index], gm.indexCount, 1, 0, 0, 0);
        }

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
    for (auto& gm : gpuMeshes) {
        vkDestroyImageView(device, gm.textureView, NULL);
        vkDestroyImage(device, gm.textureImage, NULL);
        vkFreeMemory(device, gm.textureMemory, NULL);
        vkDestroyBuffer(device, gm.vertexBuffer, NULL);
        vkFreeMemory(device, gm.vertexMemory, NULL);
        vkDestroyBuffer(device, gm.indexBuffer, NULL);
        vkFreeMemory(device, gm.indexMemory, NULL);
    }

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

