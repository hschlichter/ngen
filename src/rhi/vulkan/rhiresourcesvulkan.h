#pragma once

#include "rhitypes.h"

#include <string>
#include <vulkan/vulkan.h>

struct RhiBufferVulkan : public RhiBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct RhiTextureVulkan : public RhiTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
};

struct RhiSamplerVulkan : public RhiSampler {
    VkSampler sampler = VK_NULL_HANDLE;
};

struct RhiShaderModuleVulkan : public RhiShaderModule {
    VkShaderModule module = VK_NULL_HANDLE;
    std::string entryPoint;
};

struct RhiPipelineVulkan : public RhiPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
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

inline auto toRhiError(VkResult result) -> RhiError {
    switch (result) {
        case VK_ERROR_OUT_OF_DATE_KHR:
            return RhiError::OutOfDate;
        case VK_SUBOPTIMAL_KHR:
            return RhiError::Suboptimal;
        case VK_ERROR_DEVICE_LOST:
            return RhiError::DeviceLost;
        default:
            return RhiError::Failed;
    }
}
