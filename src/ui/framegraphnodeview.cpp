#include "framegraphnodeview.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr float kNodeWidthPass = 140.0f;
constexpr float kNodeHeightPass = 56.0f;
constexpr float kNodeWidthResource = 160.0f;
constexpr float kNodeHeightResourceNoThumb = 44.0f;
constexpr float kThumbMaxHeight = 64.0f;
constexpr float kColumnStride = 220.0f;
constexpr float kRowGap = 18.0f;
constexpr float kMarginX = 40.0f;
constexpr float kMarginY = 40.0f;

constexpr float kZoomMin = 0.3f;
constexpr float kZoomMax = 2.5f;
constexpr float kZoomStep = 1.15f;

enum class NodeKind : uint8_t {
    Pass,
    Resource
};

struct LaidOutNode {
    NodeKind kind = NodeKind::Pass;
    uint32_t index = 0;
    int column = 0;
    ImVec2 size{0, 0};
    ImVec2 pos{0, 0}; // canvas-space top-left
};

struct Layout {
    std::vector<LaidOutNode> nodes;
    std::unordered_map<uint64_t, uint32_t> lookup;
};

auto encode(NodeKind k, uint32_t i) -> uint64_t {
    return ((uint64_t) (uint8_t) k << 32) | i;
}

auto resourceSize(const FgResourceDebug& r) -> ImVec2 {
    if (r.previewTextureId != 0 && r.previewWidth > 0 && r.previewHeight > 0) {
        auto aspect = (float) r.previewWidth / (float) r.previewHeight;
        float thumbW = std::min(kNodeWidthResource - 12.0f, kThumbMaxHeight * aspect);
        float thumbH = thumbW / aspect;
        return {kNodeWidthResource, thumbH + kNodeHeightResourceNoThumb};
    }
    return {kNodeWidthResource, kNodeHeightResourceNoThumb};
}

auto buildLayout(const FrameGraphDebugSnapshot& snap) -> Layout {
    Layout layout;
    auto passCount = (uint32_t) snap.passes.size();
    auto resCount = (uint32_t) snap.resources.size();
    if (passCount == 0) {
        return layout;
    }

    int maxCol = 0;
    for (uint32_t p = 0; p < passCount; p++) {
        auto exec = snap.passes[p].executionIndex;
        int col = exec != UINT32_MAX ? (int) (exec * 2) : (int) (snap.executionOrder.size() * 2);
        LaidOutNode n;
        n.kind = NodeKind::Pass;
        n.index = p;
        n.column = col;
        n.size = {kNodeWidthPass, kNodeHeightPass};
        layout.nodes.push_back(n);
        maxCol = std::max(maxCol, col);
    }

    for (uint32_t r = 0; r < resCount; r++) {
        const auto& res = snap.resources[r];
        int col = 1;
        if (res.producerPass != UINT32_MAX) {
            auto exec = snap.passes[res.producerPass].executionIndex;
            col = exec != UINT32_MAX ? (int) (exec * 2 + 1) : 1;
        } else if (!res.consumerPasses.empty()) {
            auto exec = snap.passes[res.consumerPasses.front()].executionIndex;
            col = exec != UINT32_MAX ? std::max(0, (int) (exec * 2) - 1) : 1;
        }
        LaidOutNode n;
        n.kind = NodeKind::Resource;
        n.index = r;
        n.column = col;
        n.size = resourceSize(res);
        layout.nodes.push_back(n);
        maxCol = std::max(maxCol, col);
    }

    std::vector<float> columnCursor((size_t) maxCol + 1, kMarginY);
    for (auto& n : layout.nodes) {
        n.pos.x = kMarginX + (float) n.column * kColumnStride;
        n.pos.y = columnCursor[(size_t) n.column];
        columnCursor[(size_t) n.column] += n.size.y + kRowGap;
    }

    for (uint32_t i = 0; i < layout.nodes.size(); i++) {
        layout.lookup.emplace(encode(layout.nodes[i].kind, layout.nodes[i].index), i);
    }

    std::vector<std::vector<uint32_t>> byColumn((size_t) maxCol + 1);
    for (uint32_t i = 0; i < layout.nodes.size(); i++) {
        byColumn[(size_t) layout.nodes[i].column].push_back(i);
    }

    auto centerY = [&](uint32_t nodeIdx) -> float {
        return layout.nodes[nodeIdx].pos.y + layout.nodes[nodeIdx].size.y * 0.5f;
    };

    auto neighbors = [&](uint32_t nodeIdx, bool lookLeft) -> std::vector<uint32_t> {
        std::vector<uint32_t> out;
        const auto& node = layout.nodes[nodeIdx];
        if (node.kind == NodeKind::Pass) {
            const auto& pass = snap.passes[node.index];
            const auto& list = lookLeft ? pass.reads : pass.writes;
            for (const auto& a : list) {
                auto it = layout.lookup.find(encode(NodeKind::Resource, a.resourceIndex));
                if (it != layout.lookup.end()) {
                    out.push_back(it->second);
                }
            }
        } else {
            const auto& res = snap.resources[node.index];
            if (lookLeft) {
                if (res.producerPass != UINT32_MAX) {
                    auto it = layout.lookup.find(encode(NodeKind::Pass, res.producerPass));
                    if (it != layout.lookup.end()) {
                        out.push_back(it->second);
                    }
                }
            } else {
                for (auto c : res.consumerPasses) {
                    auto it = layout.lookup.find(encode(NodeKind::Pass, c));
                    if (it != layout.lookup.end()) {
                        out.push_back(it->second);
                    }
                }
            }
        }
        return out;
    };

    auto sweep = [&](bool leftToRight) {
        int start = leftToRight ? 1 : maxCol - 1;
        int end = leftToRight ? maxCol + 1 : -1;
        int step = leftToRight ? 1 : -1;
        for (int c = start; c != end; c += step) {
            for (auto idx : byColumn[(size_t) c]) {
                auto ns = neighbors(idx, leftToRight);
                if (ns.empty()) {
                    continue;
                }
                float sum = 0.0f;
                for (auto n : ns) {
                    sum += centerY(n);
                }
                float mean = sum / (float) ns.size();
                layout.nodes[idx].pos.y = mean - layout.nodes[idx].size.y * 0.5f;
            }
        }
    };

    sweep(true);
    sweep(false);

    for (auto& column : byColumn) {
        std::sort(column.begin(), column.end(), [&](uint32_t a, uint32_t b) { return layout.nodes[a].pos.y < layout.nodes[b].pos.y; });
        float minY = kMarginY;
        for (auto idx : column) {
            auto& n = layout.nodes[idx];
            n.pos.y = std::max(n.pos.y, minY);
            minY = n.pos.y + n.size.y + kRowGap;
        }
    }

    return layout;
}

auto accessColor(FgAccessFlags a) -> ImU32 {
    switch (std::to_underlying(a)) {
        case std::to_underlying(FgAccessFlags::ColorAttachment):
            return IM_COL32(51, 153, 76, 255);
        case std::to_underlying(FgAccessFlags::DepthAttachment):
            return IM_COL32(140, 89, 38, 255);
        case std::to_underlying(FgAccessFlags::ShaderRead):
            return IM_COL32(51, 102, 191, 255);
        case std::to_underlying(FgAccessFlags::TransferSrc):
        case std::to_underlying(FgAccessFlags::TransferDst):
            return IM_COL32(153, 102, 25, 255);
        case std::to_underlying(FgAccessFlags::Present):
            return IM_COL32(127, 51, 140, 255);
        default:
            return IM_COL32(102, 102, 102, 255);
    }
}

void drawPassNode(ImDrawList* dl, const FgPassDebug& pass, ImVec2 pos, ImVec2 size, bool selected, float zoom) {
    ImU32 fill = pass.culled ? IM_COL32(44, 40, 52, 255) : IM_COL32(38, 52, 80, 255);
    ImU32 accent = IM_COL32(110, 150, 220, 255);
    if (pass.culled) {
        accent = IM_COL32(90, 85, 100, 255);
    } else if (pass.hasSideEffects) {
        accent = IM_COL32(220, 90, 70, 255); // red-ish: this pass has observable side effects
    }
    ImU32 border = IM_COL32(10, 12, 18, 255);
    float borderThickness = 1.0f * zoom;
    if (selected) {
        border = IM_COL32(255, 200, 100, 255);
        borderThickness = 3.0f * zoom;
    }

    float rounding = 3.0f * zoom;
    ImVec2 br{pos.x + size.x, pos.y + size.y};
    dl->AddRectFilled(pos, br, fill, rounding);

    // Left accent bar — distinctive "pass" marker; its color encodes side-effects / culled.
    float accentWidth = 4.0f * zoom;
    dl->AddRectFilled(pos, {pos.x + accentWidth, pos.y + size.y}, accent, rounding, ImDrawFlags_RoundCornersLeft);

    dl->AddRect(pos, br, border, rounding, 0, borderThickness);

    auto* font = ImGui::GetFont();
    float nameFontSize = 14.0f * zoom;
    float subFontSize = 11.0f * zoom;
    ImU32 textColor = pass.culled ? IM_COL32(150, 150, 160, 255) : IM_COL32_WHITE;

    if (zoom >= 0.5f) {
        dl->AddText(font, nameFontSize, {pos.x + 10.0f * zoom, pos.y + 8.0f * zoom}, textColor, pass.name.c_str());

        char execLine[32];
        if (pass.executionIndex != UINT32_MAX) {
            std::snprintf(execLine, sizeof(execLine), "exec #%u", pass.executionIndex);
        } else {
            std::snprintf(execLine, sizeof(execLine), "not scheduled");
        }
        dl->AddText(font, subFontSize, {pos.x + 10.0f * zoom, pos.y + size.y - 18.0f * zoom}, IM_COL32(170, 180, 200, 255), execLine);
    }
}

void drawResourceNode(ImDrawList* dl, const FgResourceDebug& res, ImVec2 pos, ImVec2 size, bool selected, float zoom) {
    ImU32 fill = IM_COL32(68, 58, 52, 255);
    ImU32 border = IM_COL32(10, 10, 12, 255);
    float borderThickness = 1.0f * zoom;
    if (res.external) {
        border = IM_COL32(90, 160, 230, 255);
        borderThickness = 2.0f * zoom;
    }
    if (selected) {
        border = IM_COL32(255, 200, 100, 255);
        borderThickness = 3.0f * zoom;
    }

    float rounding = 10.0f * zoom;
    ImVec2 br{pos.x + size.x, pos.y + size.y};
    dl->AddRectFilled(pos, br, fill, rounding);
    dl->AddRect(pos, br, border, rounding, 0, borderThickness);

    float textY = pos.y + 6.0f * zoom;
    if (res.previewTextureId != 0 && res.previewWidth > 0 && res.previewHeight > 0) {
        auto aspect = (float) res.previewWidth / (float) res.previewHeight;
        float thumbW = std::min(size.x - 12.0f * zoom, kThumbMaxHeight * aspect * zoom);
        float thumbH = thumbW / aspect;
        ImVec2 thumbPos{pos.x + (size.x - thumbW) * 0.5f, pos.y + 6.0f * zoom};
        dl->AddImageRounded(
            (ImTextureID) res.previewTextureId, thumbPos, {thumbPos.x + thumbW, thumbPos.y + thumbH}, {0, 0}, {1, 1}, IM_COL32_WHITE, 4.0f * zoom);
        textY = thumbPos.y + thumbH + 4.0f * zoom;
    }

    if (zoom >= 0.5f) {
        auto* font = ImGui::GetFont();
        float nameFontSize = 13.0f * zoom;
        float dimFontSize = 10.0f * zoom;
        const char* nameText = res.name.empty() ? "(unnamed)" : res.name.c_str();
        dl->AddText(font, nameFontSize, {pos.x + 8.0f * zoom, textY}, IM_COL32_WHITE, nameText);
        char dimLine[64];
        std::snprintf(dimLine, sizeof(dimLine), "%ux%u %s", res.width, res.height, res.formatName);
        dl->AddText(font, dimFontSize, {pos.x + 8.0f * zoom, textY + 15.0f * zoom}, IM_COL32(180, 170, 160, 255), dimLine);
    }
}

void drawEdge(ImDrawList* dl, ImVec2 from, ImVec2 to, FgAccessFlags access, bool highlighted, float zoom) {
    ImU32 color = accessColor(access);
    float thickness = (highlighted ? 3.0f : 2.0f) * zoom;
    if (!highlighted) {
        color = (color & 0x00FFFFFF) | 0xC0000000;
    }
    float dx = to.x - from.x;
    ImVec2 cp1{from.x + dx * 0.4f, from.y};
    ImVec2 cp2{to.x - dx * 0.4f, to.y};
    dl->AddBezierCubic(from, cp1, cp2, to, color, thickness);
}

auto rightEdge(const LaidOutNode& n) -> ImVec2 {
    return {n.pos.x + n.size.x, n.pos.y + n.size.y * 0.5f};
}
auto leftEdge(const LaidOutNode& n) -> ImVec2 {
    return {n.pos.x, n.pos.y + n.size.y * 0.5f};
}

auto hitTest(const Layout& layout, ImVec2 pointCanvas) -> std::optional<uint32_t> {
    for (size_t i = layout.nodes.size(); i > 0; i--) {
        const auto& n = layout.nodes[i - 1];
        if (pointCanvas.x >= n.pos.x && pointCanvas.x <= n.pos.x + n.size.x && pointCanvas.y >= n.pos.y && pointCanvas.y <= n.pos.y + n.size.y) {
            return (uint32_t) (i - 1);
        }
    }
    return std::nullopt;
}

} // namespace

void drawFrameGraphNodeView(const FrameGraphDebugSnapshot& snap, std::optional<uint32_t>& selPass, std::optional<uint32_t>& selResource) {
    auto layout = buildLayout(snap);

    static ImVec2 scroll{0.0f, 0.0f};
    static float zoom = 1.0f;
    static bool panFromLeftDrag = false;

    ImGui::BeginChild("##fg_graph_canvas", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
    ImVec2 canvasScreenOrigin = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    auto* dl = ImGui::GetWindowDrawList();

    ImGui::InvisibleButton("##fg_canvas_hit", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();

    auto canvasToScreen = [&](ImVec2 p) -> ImVec2 {
        return {canvasScreenOrigin.x + scroll.x + p.x * zoom, canvasScreenOrigin.y + scroll.y + p.y * zoom};
    };
    auto screenToCanvas = [&](ImVec2 p) -> ImVec2 {
        return {(p.x - canvasScreenOrigin.x - scroll.x) / zoom, (p.y - canvasScreenOrigin.y - scroll.y) / zoom};
    };

    // Zoom on scroll, centered at the mouse so the cursor stays over the same world point.
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        float wheel = ImGui::GetIO().MouseWheel;
        float target = zoom * (wheel > 0 ? kZoomStep : 1.0f / kZoomStep);
        float newZoom = std::clamp(target, kZoomMin, kZoomMax);
        if (newZoom != zoom) {
            ImVec2 mouseScreen = ImGui::GetMousePos();
            ImVec2 anchor = screenToCanvas(mouseScreen);
            zoom = newZoom;
            scroll.x = mouseScreen.x - canvasScreenOrigin.x - anchor.x * zoom;
            scroll.y = mouseScreen.y - canvasScreenOrigin.y - anchor.y * zoom;
        }
    }

    // Edges first (under nodes).
    for (uint32_t p = 0; p < snap.passes.size(); p++) {
        const auto& pass = snap.passes[p];
        auto passEntry = layout.lookup.find(encode(NodeKind::Pass, p));
        if (passEntry == layout.lookup.end()) {
            continue;
        }
        const auto& passNode = layout.nodes[passEntry->second];

        for (const auto& r : pass.reads) {
            auto resEntry = layout.lookup.find(encode(NodeKind::Resource, r.resourceIndex));
            if (resEntry == layout.lookup.end()) {
                continue;
            }
            const auto& resNode = layout.nodes[resEntry->second];
            bool highlighted = (selPass && *selPass == p) || (selResource && *selResource == r.resourceIndex);
            drawEdge(dl, canvasToScreen(rightEdge(resNode)), canvasToScreen(leftEdge(passNode)), r.access, highlighted, zoom);
        }
        for (const auto& w : pass.writes) {
            auto resEntry = layout.lookup.find(encode(NodeKind::Resource, w.resourceIndex));
            if (resEntry == layout.lookup.end()) {
                continue;
            }
            const auto& resNode = layout.nodes[resEntry->second];
            bool highlighted = (selPass && *selPass == p) || (selResource && *selResource == w.resourceIndex);
            drawEdge(dl, canvasToScreen(rightEdge(passNode)), canvasToScreen(leftEdge(resNode)), w.access, highlighted, zoom);
        }
    }

    for (const auto& n : layout.nodes) {
        auto screenPos = canvasToScreen(n.pos);
        ImVec2 screenSize{n.size.x * zoom, n.size.y * zoom};
        if (n.kind == NodeKind::Pass) {
            bool selected = selPass && *selPass == n.index;
            drawPassNode(dl, snap.passes[n.index], screenPos, screenSize, selected, zoom);
        } else {
            bool selected = selResource && *selResource == n.index;
            drawResourceNode(dl, snap.resources[n.index], screenPos, screenSize, selected, zoom);
        }
    }

    // Interaction.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto mouseCanvas = screenToCanvas(ImGui::GetMousePos());
        if (auto hit = hitTest(layout, mouseCanvas)) {
            const auto& n = layout.nodes[*hit];
            if (n.kind == NodeKind::Pass) {
                selPass = n.index;
            } else {
                selResource = n.index;
            }
            panFromLeftDrag = false;
        } else {
            panFromLeftDrag = true;
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        panFromLeftDrag = false;
    }

    bool middleDrag = active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle);
    bool leftDrag = active && panFromLeftDrag && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    if (middleDrag || leftDrag) {
        scroll.x += ImGui::GetIO().MouseDelta.x;
        scroll.y += ImGui::GetIO().MouseDelta.y;
    }

    // Overlay: zoom readout + hint.
    char zoomText[32];
    std::snprintf(zoomText, sizeof(zoomText), "%.0f%%", zoom * 100.0f);
    dl->AddText({canvasScreenOrigin.x + 8.0f, canvasScreenOrigin.y + 6.0f}, IM_COL32(180, 180, 180, 200), zoomText);

    ImGui::EndChild();
}
