#pragma once

#include "imguibackend.h"

#include <vulkan/vulkan.h>

#include <mutex>

union SDL_Event;
struct SDL_Window;

// ImGui on top of the Vulkan backend via imgui_impl_vulkan and imgui_impl_sdl3.
// Application-level: knows SDL and Vulkan, sits next to main.cpp.
class ImGuiBackendVulkan : public ImGuiBackend {
public:
    explicit ImGuiBackendVulkan(SDL_Window* window) : window(window) {}

    auto init(const ImGuiBackendInitInfo& info) -> void override;
    auto processEvent(const SDL_Event* event) -> bool;
    auto beginFrame() -> void override;
    auto endFrame() -> ImGuiFrameSnapshot override;
    auto renderDrawData(RhiCommandBuffer* cmd, ImGuiFrameSnapshot& snapshot) -> void override;
    auto shutdown() -> void override;
    auto registerTexture(RhiTexture* texture, RhiSampler* sampler) -> uint64_t override;
    auto unregisterTexture(uint64_t id) -> void override;

private:
    SDL_Window* window = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkDescriptorPool imguiPool = VK_NULL_HANDLE;
    bool srgbTarget = false; // imgui's sRGB colours are linearised for an sRGB target
    // ImGui's textures (the font atlas) are built on the main thread and uploaded on the render thread. The main thread holds the lock from
    // beginFrame to endFrame, while ImGui may add glyphs; the render thread takes it to upload a frame's texture changes, only for a frame
    // that had some. Without it, glyphs added during an upload were marked uploaded with it and never drawn.
    std::mutex textureMutex;
    std::unique_lock<std::mutex> frameLock;
};
