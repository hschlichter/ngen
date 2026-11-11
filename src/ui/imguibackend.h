#pragma once

#include "previewtextures.h"
#include "rhitypes.h"

#include <vector>

class RhiDevice;
class RhiCommandBuffer;
struct ImDrawData;
struct ImDrawList;
struct ImTextureData;
template <typename T>
struct ImVector;

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
    ImVector<ImTextureData*>* textures = nullptr; // ImGui's live texture list, shared with the main thread
    bool texturesChanged = false;                 // a texture wanted creating, updating or destroying when the frame was built

    ImGuiFrameSnapshot() = default;
    ImGuiFrameSnapshot(const ImGuiFrameSnapshot&) = delete;
    ImGuiFrameSnapshot& operator=(const ImGuiFrameSnapshot&) = delete;
    ImGuiFrameSnapshot(ImGuiFrameSnapshot&& other) noexcept;
    ImGuiFrameSnapshot& operator=(ImGuiFrameSnapshot&& other) noexcept;
    ~ImGuiFrameSnapshot();

    // Copies the frame's draw lists. With linearizeColors the vertex colours are converted from sRGB to linear: imgui's
    // colours are sRGB values, and an sRGB render target would otherwise encode them a second time and wash them out.
    void cloneFrom(const ImDrawData* drawData, bool linearizeColors);
    void fillDrawData(ImDrawData& out) const;
};

struct ImGuiBackendInitInfo {
    RhiDevice* device;
    RhiFormat colorFormat;
    uint32_t imageCount;
};

// Renders ImGui draw data through the RHI, in the UI's render extension (UiRender), and shows the renderer's previews (PreviewTextures:
// an id is an ImTextureID for ImGui::Image). The application owns the concrete backend (ImGuiBackendVulkan) and the window and its events.
class ImGuiBackend : public PreviewTextures {
public:
    virtual auto init(const ImGuiBackendInitInfo& info) -> void = 0;
    virtual auto beginFrame() -> void = 0;
    virtual auto endFrame() -> ImGuiFrameSnapshot = 0;
    virtual auto renderDrawData(RhiCommandBuffer* cmd, ImGuiFrameSnapshot& snapshot) -> void = 0;
    virtual auto shutdown() -> void = 0;
};
