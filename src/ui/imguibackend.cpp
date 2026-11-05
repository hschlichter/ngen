#include "imguibackend.h"

#include <imgui.h>

#include <array>
#include <cmath>
#include <cstdint>

namespace {

// sRGB-encoded byte to linear byte, rounded.
auto srgbToLinearTable() -> const std::array<uint8_t, 256>& {
    static const std::array<uint8_t, 256> table = [] {
        std::array<uint8_t, 256> t{};
        for (int i = 0; i < 256; i++) {
            float c = (float) i / 255.0f;
            float linear = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            t[i] = (uint8_t) std::lround(linear * 255.0f);
        }
        return t;
    }();
    return table;
}

// Converts the colour channels of every vertex; alpha stays as it is.
auto linearizeVertexColors(ImDrawList* list) -> void {
    const auto& table = srgbToLinearTable();
    for (auto& vertex : list->VtxBuffer) {
        auto r = (uint8_t) ((vertex.col >> IM_COL32_R_SHIFT) & 0xff);
        auto g = (uint8_t) ((vertex.col >> IM_COL32_G_SHIFT) & 0xff);
        auto b = (uint8_t) ((vertex.col >> IM_COL32_B_SHIFT) & 0xff);
        auto a = (vertex.col >> IM_COL32_A_SHIFT) & 0xff;
        vertex.col = ((ImU32) table[r] << IM_COL32_R_SHIFT) | ((ImU32) table[g] << IM_COL32_G_SHIFT) | ((ImU32) table[b] << IM_COL32_B_SHIFT) |
                     (a << IM_COL32_A_SHIFT);
    }
}

} // namespace

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

void ImGuiFrameSnapshot::cloneFrom(const ImDrawData* drawData, bool linearizeColors) {
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
        if (linearizeColors) {
            linearizeVertexColors(cmdLists.back());
        }
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
