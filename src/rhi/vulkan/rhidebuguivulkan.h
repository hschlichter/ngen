#pragma once

#include "rhidebugui.h"

#include <vulkan/vulkan.h>

class RhiDebugUIVulkan : public RhiDebugUI {
public:
    auto init(const RhiDebugUIInitInfo& info) -> void override;
    auto processEvent(SDL_Event* event) -> bool override;
    auto render(RhiCommandBuffer* cmd) -> void override;
    auto shutdown() -> void override;

private:
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkDescriptorPool imguiPool = VK_NULL_HANDLE;
};
