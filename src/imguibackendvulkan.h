#pragma once

#include "imguibackend.h"

#include <vulkan/vulkan.h>

union SDL_Event;
struct SDL_Window;

// ImGui on top of the Vulkan backend via imgui_impl_vulkan and imgui_impl_sdl3.
// Application-level: knows SDL and Vulkan, sits next to main.cpp.
class ImGuiBackendVulkan : public ImGuiBackend {
public:
    explicit ImGuiBackendVulkan(SDL_Window* window) : window(window) {}

    auto init(const ImGuiBackendInitInfo& info) -> void override;
    auto processEvent(SDL_Event* event) -> bool;
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
};
