#include "rhiswapchainvulkan.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#include <algorithm>
#include <print>

auto RhiSwapchainVulkan::findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) -> uint32_t {
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

auto RhiSwapchainVulkan::init(VkPhysicalDevice physicalDevice, VkDevice device, VkSurfaceKHR surface, uint32_t queueFamilyIndex, SDL_Window* window)
    -> std::expected<void, int> {
    vkDevice = device;
    VkResult result = VK_SUCCESS;

    VkSurfaceCapabilitiesKHR capabilities;
    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }

    uint32_t formatCount = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }

    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }

    auto format = formats[0];

    {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        ext.width = (uint32_t) w;
        ext.height = (uint32_t) h;
    }
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
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };

    result = vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &swapchain);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateSwapchainKHR failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }

    images.resize(imgCount);
    result = vkGetSwapchainImagesKHR(device, swapchain, &imgCount, images.data());
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }

    imageViews.resize(imgCount);
    for (uint32_t i = 0; i < imgCount; i++) {
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

        result = vkCreateImageView(device, &viewInfo, nullptr, &imageViews[i]);
        if (result != VK_SUCCESS) {
            std::println(stderr, "vkCreateImageView failed: {}({})", string_VkResult(result), (int) result);
            return std::unexpected(1);
        }
    }

    // Depth image
    auto depthFormat = VK_FORMAT_D32_SFLOAT;

    VkImageCreateInfo depthImageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = depthFormat,
        .extent = {ext.width, ext.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    result = vkCreateImage(device, &depthImageInfo, nullptr, &depthImage);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateImage failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }

    VkMemoryRequirements depthMemReqs;
    vkGetImageMemoryRequirements(device, depthImage, &depthMemReqs);

    auto depthMemType = findMemoryType(physicalDevice, depthMemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (depthMemType == UINT32_MAX) {
        return std::unexpected(1);
    }

    VkMemoryAllocateInfo depthAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = depthMemReqs.size,
        .memoryTypeIndex = depthMemType,
    };
    result = vkAllocateMemory(device, &depthAllocInfo, nullptr, &depthMemory);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkAllocateMemory failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }
    vkBindImageMemory(device, depthImage, depthMemory, 0);

    VkImageViewCreateInfo depthViewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = depthImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = depthFormat,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    result = vkCreateImageView(device, &depthViewInfo, nullptr, &depthImageView);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateImageView failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }

    // Render pass
    VkAttachmentDescription attachments[] = { // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
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

    result = vkCreateRenderPass(device, &renderPassInfo, nullptr, &rhiRenderPass.renderPass);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateRenderPass failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }

    // Framebuffers
    rhiFramebuffers.resize(imgCount);
    for (uint32_t i = 0; i < imgCount; i++) {
        VkImageView fbAttachments[] = {imageViews[i], depthImageView}; // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
        VkFramebufferCreateInfo framebufferInfo = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = rhiRenderPass.renderPass,
            .attachmentCount = 2,
            .pAttachments = fbAttachments,
            .width = ext.width,
            .height = ext.height,
            .layers = 1,
        };

        result = vkCreateFramebuffer(device, &framebufferInfo, nullptr, &rhiFramebuffers[i].framebuffer);
        if (result != VK_SUCCESS) {
            std::println(stderr, "vkCreateFramebuffer failed: {}({})", string_VkResult(result), (int) result);
            return std::unexpected(1);
        }
    }

    return {};
}

auto RhiSwapchainVulkan::acquireNextImage(RhiSemaphore* signalSemaphore) -> std::expected<uint32_t, int> {
    auto* sem = static_cast<RhiSemaphoreVulkan*>(signalSemaphore);
    uint32_t index = 0;
    auto result = vkAcquireNextImageKHR(vkDevice, swapchain, UINT64_MAX, sem->semaphore, VK_NULL_HANDLE, &index);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkAcquireNextImageKHR failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(1);
    }
    return index;
}

auto RhiSwapchainVulkan::destroy() -> void {
    vkDestroyImageView(vkDevice, depthImageView, nullptr);
    vkDestroyImage(vkDevice, depthImage, nullptr);
    vkFreeMemory(vkDevice, depthMemory, nullptr);

    for (uint32_t i = 0; i < imgCount; i++) {
        vkDestroyFramebuffer(vkDevice, rhiFramebuffers[i].framebuffer, nullptr);
        vkDestroyImageView(vkDevice, imageViews[i], nullptr);
    }

    vkDestroyRenderPass(vkDevice, rhiRenderPass.renderPass, nullptr);
    vkDestroySwapchainKHR(vkDevice, swapchain, nullptr);
}
