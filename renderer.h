#pragma once

#include "devicevulkan.h"
#include "swapchainvulkan.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>

struct Scene;
struct Camera;

struct GpuMesh {
    VkBuffer vertexBuffer, indexBuffer;
    VkDeviceMemory vertexMemory, indexMemory;
    uint32_t indexCount;
    VkImage textureImage;
    VkDeviceMemory textureMemory;
    VkImageView textureView;
    glm::mat4 transform;
};

struct Renderer {
    VulkanDevice dev;
    VulkanSwapchain swapchain;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets;
    VkSampler textureSampler = VK_NULL_HANDLE;
    VkShaderModule vertShader = VK_NULL_HANDLE;
    VkShaderModule fragShader = VK_NULL_HANDLE;

    std::vector<GpuMesh> gpuMeshes;
    std::vector<VkBuffer> uniformBuffers;
    std::vector<VkDeviceMemory> uniformBuffersMemory;
    std::vector<void*> uniformBuffersMapped;

    std::vector<VkCommandBuffer> cmdBuffers;
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inflightFences;
    uint32_t currentFrame = 0;

    int init(SDL_Window* window);
    void uploadScene(const Scene& scene);
    void render(const Camera& camera, SDL_Window* window);
    void destroy();
};
