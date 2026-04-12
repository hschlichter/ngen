#pragma once

#include "rhieditorui.h"

#include <vulkan/vulkan.h>

class RhiEditorUIVulkan : public RhiEditorUI {
public:
    auto init(const RhiEditorUIInitInfo& info) -> void override;
    auto processEvent(SDL_Event* event) -> bool override;
    auto beginFrame() -> void override;
    auto endFrame() -> ImGuiFrameSnapshot override;
    auto renderDrawData(RhiCommandBuffer* cmd, ImGuiFrameSnapshot& snapshot) -> void override;
    auto shutdown() -> void override;

private:
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkDescriptorPool imguiPool = VK_NULL_HANDLE;
};
