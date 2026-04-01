#include "swapchainvulkan.h"
#include "devicevulkan.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#include <cstdio>

int SwapchainVulkan::init(DeviceVulkan& dev, SDL_Window* window) {
    VkResult result;

    // Query surface capabilities and formats
    VkSurfaceCapabilitiesKHR capabilities;
    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.physicalDevice, dev.surface, &capabilities);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    uint32_t formatCount;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physicalDevice, dev.surface, &formatCount, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physicalDevice, dev.surface, &formatCount, formats.data());
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    VkSurfaceFormatKHR format = formats[0];
    imageFormat = format.format;

    uint32_t presentModeCount;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physicalDevice, dev.surface, &presentModeCount, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkGetPhysicalDeviceSurfacePresentModesKHR failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physicalDevice, dev.surface, &presentModeCount, presentModes.data());
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkGetPhysicalDeviceSurfacePresentModesKHR failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

    // Determine extent
    {
        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        extent.width = (uint32_t)w;
        extent.height = (uint32_t)h;
    }
    if (extent.width < capabilities.minImageExtent.width) extent.width = capabilities.minImageExtent.width;
    if (extent.width > capabilities.maxImageExtent.width) extent.width = capabilities.maxImageExtent.width;
    if (extent.height < capabilities.minImageExtent.height) extent.height = capabilities.minImageExtent.height;
    if (extent.height > capabilities.maxImageExtent.height) extent.height = capabilities.maxImageExtent.height;
    printf("Swapchain extent: %dx%d\n", extent.width, extent.height);

    imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    // Create swapchain
    VkSwapchainCreateInfoKHR swapchainInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = dev.surface,
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

    result = vkCreateSwapchainKHR(dev.device, &swapchainInfo, NULL, &swapchain);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateSwapchainKHR failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    images.resize(imageCount);
    result = vkGetSwapchainImagesKHR(dev.device, swapchain, &imageCount, images.data());

    // Create image views
    imageViews.resize(imageCount);
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

        result = vkCreateImageView(dev.device, &viewInfo, NULL, &imageViews[i]);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkCreateImageView failed: %s(%d)\n", string_VkResult(result), result);
            return 1;
        }
    }

    // Create depth image
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

    result = vkCreateImage(dev.device, &depthImageInfo, NULL, &depthImage);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateImage failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    VkMemoryRequirements depthMemReqs;
    vkGetImageMemoryRequirements(dev.device, depthImage, &depthMemReqs);

    uint32_t depthMemType = dev.findMemoryType(depthMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (depthMemType == UINT32_MAX) return 1;

    VkMemoryAllocateInfo depthAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = depthMemReqs.size,
        .memoryTypeIndex = depthMemType,
    };
    result = vkAllocateMemory(dev.device, &depthAllocInfo, NULL, &depthMemory);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateMemory failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }
    vkBindImageMemory(dev.device, depthImage, depthMemory, 0);

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
    result = vkCreateImageView(dev.device, &depthViewInfo, NULL, &depthImageView);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateImageView failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    // Create render pass
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

    result = vkCreateRenderPass(dev.device, &renderPassInfo, NULL, &renderPass);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateRenderPass failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    // Create framebuffers
    framebuffers.resize(imageCount);
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

        result = vkCreateFramebuffer(dev.device, &framebufferInfo, NULL, &framebuffers[i]);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkCreateFramebuffer failed: %s(%d)\n", string_VkResult(result), result);
            return 1;
        }
    }

    return 0;
}

void SwapchainVulkan::destroy(VkDevice device) {
    vkDestroyImageView(device, depthImageView, NULL);
    vkDestroyImage(device, depthImage, NULL);
    vkFreeMemory(device, depthMemory, NULL);

    for (uint32_t i = 0; i < imageCount; i++) {
        vkDestroyFramebuffer(device, framebuffers[i], NULL);
        vkDestroyImageView(device, imageViews[i], NULL);
    }

    vkDestroyRenderPass(device, renderPass, NULL);
    vkDestroySwapchainKHR(device, swapchain, NULL);
}
