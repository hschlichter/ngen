#pragma once

#include "renderer.h"
#include "devicevulkan.h"
#include "swapchainvulkan.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>

struct UniformBufferObject {
    glm::mat4 view;
    glm::mat4 proj;
};

struct GpuMesh {
    VkBuffer vertexBuffer, indexBuffer;
    VkDeviceMemory vertexMemory, indexMemory;
    uint32_t indexCount;
    VkImage textureImage;
    VkDeviceMemory textureMemory;
    VkImageView textureView;
    glm::mat4 transform;
};

class RendererVulkan : public Renderer {
public:
    RendererVulkan() = default;
    ~RendererVulkan() override = default;

    int init(SDL_Window* window) override;
    void uploadScene(const Scene& scene) override;
    void draw(const Camera& camera, SDL_Window* window) override;
    void destroy() override;

private:
    DeviceVulkan dev;
    SwapchainVulkan swapchain;

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
};
