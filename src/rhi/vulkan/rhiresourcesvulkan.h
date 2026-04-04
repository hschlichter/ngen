#pragma once

#include "rhitypes.h"

#include <vulkan/vulkan.h>

struct RhiBufferVulkan : public RhiBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct RhiTextureVulkan : public RhiTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct RhiSamplerVulkan : public RhiSampler {
    VkSampler sampler = VK_NULL_HANDLE;
};

struct RhiShaderModuleVulkan : public RhiShaderModule {
    VkShaderModule module = VK_NULL_HANDLE;
};

struct RhiPipelineVulkan : public RhiPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

struct RhiDescriptorSetLayoutVulkan : public RhiDescriptorSetLayout {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
};

struct RhiDescriptorPoolVulkan : public RhiDescriptorPool {
    VkDescriptorPool pool = VK_NULL_HANDLE;
};

struct RhiDescriptorSetVulkan : public RhiDescriptorSet {
    VkDescriptorSet set = VK_NULL_HANDLE;
};

struct RhiSemaphoreVulkan : public RhiSemaphore {
    VkSemaphore semaphore = VK_NULL_HANDLE;
};

struct RhiFenceVulkan : public RhiFence {
    VkFence fence = VK_NULL_HANDLE;
};
