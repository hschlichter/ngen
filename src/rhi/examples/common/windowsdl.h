#pragma once

#include "rhiwindow.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <print>

// Build an RhiWindow from an SDL window. Example-side glue: the RHI never sees SDL.
// Header-only so each example is a single translation unit. main.cpp carries its
// own copy so the engine does not depend on example code.
inline auto makeRhiWindowSdl(SDL_Window* window) -> RhiWindow {
    RhiWindow rhiWindow;

    uint32_t extensionCount = 0;
    const auto* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    rhiWindow.instanceExtensions.assign(extensions, extensions + extensionCount);

    rhiWindow.createSurface = [window](void* nativeInstance, void** nativeSurface) -> bool {
        auto instance = (VkInstance) nativeInstance;
        auto* surface = (VkSurfaceKHR*) nativeSurface;
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, surface)) {
            std::println(stderr, "SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
            return false;
        }
        return true;
    };

    return rhiWindow;
}
