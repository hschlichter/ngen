#include "vulkan/vulkan_core.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#include <stdint.h>
#include <stdio.h>


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
// 12. Main loop
// 13. Clean up

int main(int argc, char* argv[]) {
    // 1. SDL init and window stuff.
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_PropertiesID windowProps = SDL_CreateProperties();
    SDL_SetStringProperty(windowProps, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "ngen");
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 1280);
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 720);
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
        .pEngineName = "No Engine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = apiVersion,
    };

    uint32_t extensionsCount = 0;
    char const* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionsCount);
    for (uint32_t i = 0; i < extensionsCount; i++) {
        printf("%s\n", extensions[i]);
    }

    VkInstanceCreateInfo instanceCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = extensionsCount,
        .ppEnabledExtensionNames = extensions,
        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR, // Needed for MoltenVk/macOS
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
    vkEnumeratePhysicalDevices(instance, &deviceCount, NULL);

    VkPhysicalDevice physicalDevices[deviceCount];
    vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices);

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = UINT32_MAX;

    for (uint32_t i = 0; i < deviceCount; i++) {
        uint32_t queueCount;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[i], &queueCount, NULL);

        VkQueueFamilyProperties props[queueCount];
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[i], &queueCount, props);

        for (uint32_t j = 0; j < queueCount; j++) {
            uint32_t presentSupport;
            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevices[i], j, surface, &presentSupport);

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
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueFamilyIndex = queueFamilyIndex,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };

    VkDeviceCreateInfo deviceCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueCreateInfo,
    };

    VkDevice device;
    vkCreateDevice(physicalDevice, &deviceCreateInfo, NULL, &device);
    
    VkQueue graphicsQueue;
    vkGetDeviceQueue(device, queueFamilyIndex, 0, &graphicsQueue);


    // 5. Create Swapchain
    VkSurfaceCapabilitiesKHR capabilities;
    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: %s(%d)\n", string_VkResult(result), result);
        return 1;
    }

    // 6. Create Image Views

    // 7. Create Render Pass
   
    // 8. Create Framebuffers
   
    // 9. Create Shader Modules
   
    // 10. Create Graphics Pipeline
   
    // 11. Allocate Command Buffers

    // 12. Main loop
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


                // Destroyg framebuffers, pipelie, images views and swapchain.
                // Re-query surface capabilities.
                // Create new swapchain.
                // Create new image views.
                // New command buffers.
            }
        }
    }


    // 13. Clean up.
    vkDestroyInstance(instance, NULL);

    SDL_DestroyWindow(window); 
    SDL_Quit();

    return 0;
}
