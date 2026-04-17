#pragma once

#include "rhitypes.h"

#include <vector>

class RhiDevice;
class RhiCommandBuffer;
union SDL_Event;
struct SDL_Window;
struct ImDrawData;
struct ImDrawList;
struct ImTextureData;
template <typename T> struct ImVector;

struct ImGuiFrameSnapshot {
    bool valid = false;
    int totalIdxCount = 0;
    int totalVtxCount = 0;
    float displayPosX = 0;
    float displayPosY = 0;
    float displaySizeX = 0;
    float displaySizeY = 0;
    float framebufferScaleX = 0;
    float framebufferScaleY = 0;
    std::vector<ImDrawList*> cmdLists;
    ImVector<ImTextureData*>* textures = nullptr;

    ImGuiFrameSnapshot() = default;
    ImGuiFrameSnapshot(const ImGuiFrameSnapshot&) = delete;
    ImGuiFrameSnapshot& operator=(const ImGuiFrameSnapshot&) = delete;
    ImGuiFrameSnapshot(ImGuiFrameSnapshot&& other) noexcept;
    ImGuiFrameSnapshot& operator=(ImGuiFrameSnapshot&& other) noexcept;
    ~ImGuiFrameSnapshot();

    void cloneFrom(const ImDrawData* drawData);
    void fillDrawData(ImDrawData& out) const;
};

struct RhiEditorUIInitInfo {
    SDL_Window* window;
    RhiDevice* device;
    RhiFormat colorFormat;
    uint32_t imageCount;
};

class RhiEditorUI {
public:
    RhiEditorUI() = default;
    RhiEditorUI(const RhiEditorUI&) = delete;
    RhiEditorUI& operator=(const RhiEditorUI&) = delete;
    RhiEditorUI(RhiEditorUI&&) = default;
    RhiEditorUI& operator=(RhiEditorUI&&) = default;
    virtual ~RhiEditorUI() = default;

    virtual auto init(const RhiEditorUIInitInfo& info) -> void = 0;
    virtual auto processEvent(SDL_Event* event) -> bool = 0;
    virtual auto beginFrame() -> void = 0;
    virtual auto endFrame() -> ImGuiFrameSnapshot = 0;
    virtual auto renderDrawData(RhiCommandBuffer* cmd, ImGuiFrameSnapshot& snapshot) -> void = 0;
    virtual auto shutdown() -> void = 0;

    // Register a sampled texture (expected in ShaderReadOnly layout) with ImGui.
    // Returns an opaque ID to be cast to ImTextureID in ImGui::Image.
    virtual auto registerTexture(RhiTexture* texture, RhiSampler* sampler) -> uint64_t = 0;
    virtual auto unregisterTexture(uint64_t id) -> void = 0;
};
