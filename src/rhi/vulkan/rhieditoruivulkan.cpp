#include "rhieditoruivulkan.h"
#include "rhicommandbuffervulkan.h"
#include "rhidevicevulkan.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL.h>

#include <print>

auto RhiEditorUIVulkan::init(const RhiEditorUIInitInfo& info) -> void {
    auto* vkDev = static_cast<RhiDeviceVulkan*>(info.device);
    vkDevice = vkDev->vkDevice();

    // Dedicated descriptor pool for ImGui
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100},
    };
    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 100,
        .poolSizeCount = 1,
        .pPoolSizes = poolSizes,
    };
    vkCreateDescriptorPool(vkDevice, &poolInfo, nullptr, &imguiPool);

    ImGui::CreateContext();

    ImGui_ImplSDL3_InitForVulkan(info.window);

    auto colorFormat = RhiDeviceVulkan::toVkFormat(info.colorFormat);

    VkPipelineRenderingCreateInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &colorFormat,
    };

    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.ApiVersion = VK_API_VERSION_1_3;
    initInfo.Instance = vkDev->vkInstance();
    initInfo.PhysicalDevice = vkDev->vkPhysicalDevice();
    initInfo.Device = vkDevice;
    initInfo.QueueFamily = vkDev->vkQueueFamilyIndex();
    initInfo.Queue = vkDev->vkGraphicsQueue();
    initInfo.DescriptorPool = imguiPool;
    initInfo.MinImageCount = info.imageCount;
    initInfo.ImageCount = info.imageCount;
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo;

    ImGui_ImplVulkan_Init(&initInfo);

    std::println("Dear ImGui initialized");
}

auto RhiEditorUIVulkan::processEvent(SDL_Event* event) -> bool {
    ImGui_ImplSDL3_ProcessEvent(event);
    auto& io = ImGui::GetIO();
    return io.WantCaptureMouse || io.WantCaptureKeyboard;
}

auto RhiEditorUIVulkan::render(RhiCommandBuffer* cmd) -> void {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (drawCallback) {
        drawCallback();
    }

    ImGui::Render();
    auto* vkCmd = static_cast<RhiCommandBufferVulkan*>(cmd);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vkCmd->cmd);
}

auto RhiEditorUIVulkan::shutdown() -> void {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (imguiPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(vkDevice, imguiPool, nullptr);
        imguiPool = VK_NULL_HANDLE;
    }
}
