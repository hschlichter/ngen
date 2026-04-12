#include "rhieditorui.h"

#include <imgui.h>

ImGuiFrameSnapshot::ImGuiFrameSnapshot(ImGuiFrameSnapshot&& other) noexcept
    : valid(other.valid)
    , totalIdxCount(other.totalIdxCount)
    , totalVtxCount(other.totalVtxCount)
    , displayPosX(other.displayPosX)
    , displayPosY(other.displayPosY)
    , displaySizeX(other.displaySizeX)
    , displaySizeY(other.displaySizeY)
    , framebufferScaleX(other.framebufferScaleX)
    , framebufferScaleY(other.framebufferScaleY)
    , cmdLists(std::move(other.cmdLists))
    , textures(other.textures) {
    other.valid = false;
    other.textures = nullptr;
}

ImGuiFrameSnapshot& ImGuiFrameSnapshot::operator=(ImGuiFrameSnapshot&& other) noexcept {
    if (this != &other) {
        for (auto* list : cmdLists) {
            IM_DELETE(list);
        }
        valid = other.valid;
        totalIdxCount = other.totalIdxCount;
        totalVtxCount = other.totalVtxCount;
        displayPosX = other.displayPosX;
        displayPosY = other.displayPosY;
        displaySizeX = other.displaySizeX;
        displaySizeY = other.displaySizeY;
        framebufferScaleX = other.framebufferScaleX;
        framebufferScaleY = other.framebufferScaleY;
        cmdLists = std::move(other.cmdLists);
        textures = other.textures;
        other.valid = false;
        other.textures = nullptr;
    }
    return *this;
}

ImGuiFrameSnapshot::~ImGuiFrameSnapshot() {
    for (auto* list : cmdLists) {
        IM_DELETE(list);
    }
}

void ImGuiFrameSnapshot::cloneFrom(const ImDrawData* drawData) {
    for (auto* list : cmdLists) {
        IM_DELETE(list);
    }
    cmdLists.clear();

    if (!drawData || !drawData->Valid) {
        valid = false;
        return;
    }

    valid = true;
    totalIdxCount = drawData->TotalIdxCount;
    totalVtxCount = drawData->TotalVtxCount;
    displayPosX = drawData->DisplayPos.x;
    displayPosY = drawData->DisplayPos.y;
    displaySizeX = drawData->DisplaySize.x;
    displaySizeY = drawData->DisplaySize.y;
    framebufferScaleX = drawData->FramebufferScale.x;
    framebufferScaleY = drawData->FramebufferScale.y;

    textures = drawData->Textures;

    cmdLists.reserve(drawData->CmdLists.Size);
    for (int i = 0; i < drawData->CmdLists.Size; i++) {
        cmdLists.push_back(drawData->CmdLists[i]->CloneOutput());
    }
}

void ImGuiFrameSnapshot::fillDrawData(ImDrawData& out) const {
    out.Valid = valid;
    out.CmdListsCount = (int) cmdLists.size();
    out.TotalIdxCount = totalIdxCount;
    out.TotalVtxCount = totalVtxCount;
    out.CmdLists.resize(0);
    for (auto* list : cmdLists) {
        out.CmdLists.push_back(list);
    }
    out.DisplayPos = ImVec2(displayPosX, displayPosY);
    out.DisplaySize = ImVec2(displaySizeX, displaySizeY);
    out.FramebufferScale = ImVec2(framebufferScaleX, framebufferScaleY);
    out.OwnerViewport = nullptr;
    out.Textures = textures;
}
