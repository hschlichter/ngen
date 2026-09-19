#pragma once

#include <functional>
#include <vector>

// Contract between the window layer and an RHI backend. The RHI never includes a
// windowing library (SDL, GLFW, native APIs); whoever owns the window fills this in
// and hands it to RhiDevice::init. Handles are backend-native and opaque here:
//
//   Vulkan: instanceExtensions = VkInstance extensions the window layer needs.
//           createSurface(VkInstance, VkSurfaceKHR*) -> bool
//   D3D12:  instanceExtensions unused.
//           createSurface(nullptr, HWND*) -> bool
//   Metal:  instanceExtensions unused.
//           createSurface(nullptr, CAMetalLayer**) -> bool
struct RhiWindow {
    std::vector<const char*> instanceExtensions;
    std::function<bool(void* nativeInstance, void** nativeSurface)> createSurface;
};
