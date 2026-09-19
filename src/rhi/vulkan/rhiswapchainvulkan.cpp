#include "rhiswapchainvulkan.h"

#include <vulkan/vk_enum_string_helper.h>

#include <algorithm>
#include <print>

auto RhiSwapchainVulkan::vkFormatToRhiFormat(VkFormat format) -> RhiFormat {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_SRGB:
            return RhiFormat::B8G8R8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_UNORM:
            return RhiFormat::B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return RhiFormat::R8G8B8A8_SRGB;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return RhiFormat::R8G8B8A8_UNORM;
        default:
            std::println(stderr, "Unsupported swapchain format: {}", (int) format);
            return RhiFormat::B8G8R8A8_SRGB;
    }
}

auto RhiSwapchainVulkan::init(VkPhysicalDevice physicalDevice, VkDevice device, VkSurfaceKHR surface, uint32_t queueFamilyIndex, RhiExtent2D extent)
    -> std::expected<void, RhiError> {
    vkPhysicalDevice = physicalDevice;
    vkDevice = device;
    vkSurface = surface;
    vkQueueFamilyIndex = queueFamilyIndex;
    VkResult result = VK_SUCCESS;

    VkSurfaceCapabilitiesKHR capabilities;
    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(RhiError::Failed);
    }

    uint32_t formatCount = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(RhiError::Failed);
    }

    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(RhiError::Failed);
    }

    auto format = formats[0];
    rhiColorFormat = vkFormatToRhiFormat(format.format);

    ext = extent;
    ext.width = std::max(ext.width, capabilities.minImageExtent.width);
    ext.width = std::min(ext.width, capabilities.maxImageExtent.width);
    ext.height = std::max(ext.height, capabilities.minImageExtent.height);
    ext.height = std::min(ext.height, capabilities.maxImageExtent.height);
    std::println("Swapchain extent: {}x{}", ext.width, ext.height);

    imgCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imgCount > capabilities.maxImageCount) {
        imgCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swapchainInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = imgCount,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = {ext.width, ext.height},
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };

    result = vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &swapchain);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateSwapchainKHR failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(RhiError::Failed);
    }

    std::vector<VkImage> images(imgCount);
    result = vkGetSwapchainImagesKHR(device, swapchain, &imgCount, images.data());
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkGetSwapchainImagesKHR failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(RhiError::Failed);
    }

    colorImages.resize(imgCount);
    for (uint32_t i = 0; i < imgCount; i++) {
        colorImages[i].image = images[i];
        colorImages[i].memory = VK_NULL_HANDLE;

        VkImageViewCreateInfo viewInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format.format,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        };

        result = vkCreateImageView(device, &viewInfo, nullptr, &colorImages[i].view);
        if (result != VK_SUCCESS) {
            std::println(stderr, "vkCreateImageView failed: {}({})", string_VkResult(result), (int) result);
            return std::unexpected(RhiError::Failed);
        }
    }

    return {};
}

auto RhiSwapchainVulkan::recreate(RhiExtent2D extent) -> std::expected<void, RhiError> {
    vkDeviceWaitIdle(vkDevice);
    destroy();
    return init(vkPhysicalDevice, vkDevice, vkSurface, vkQueueFamilyIndex, extent);
}

auto RhiSwapchainVulkan::acquireNextImage(RhiSemaphore* signalSemaphore) -> std::expected<uint32_t, RhiError> {
    auto* sem = static_cast<RhiSemaphoreVulkan*>(signalSemaphore);
    uint32_t index = 0;
    auto result = vkAcquireNextImageKHR(vkDevice, swapchain, UINT64_MAX, sem->semaphore, VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Semaphore is not signaled in this case; caller must recreate before reuse.
        return std::unexpected(RhiError::OutOfDate);
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        std::println(stderr, "vkAcquireNextImageKHR failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(toRhiError(result));
    }
    return index;
}

auto RhiSwapchainVulkan::destroy() -> void {
    for (uint32_t i = 0; i < imgCount; i++) {
        vkDestroyImageView(vkDevice, colorImages[i].view, nullptr);
    }

    vkDestroySwapchainKHR(vkDevice, swapchain, nullptr);
}
