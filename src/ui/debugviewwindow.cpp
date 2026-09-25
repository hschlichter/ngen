#include "debugviewwindow.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <imgui.h>

namespace {

// The "jet" scale of shaders/debugview.comp.
auto heat(float t) -> ImU32 {
    t = std::clamp(t, 0.0f, 1.0f);
    auto r = std::clamp(1.5f - std::abs(4.0f * t - 3.0f), 0.0f, 1.0f);
    auto g = std::clamp(1.5f - std::abs(4.0f * t - 2.0f), 0.0f, 1.0f);
    auto b = std::clamp(1.5f - std::abs(4.0f * t - 1.0f), 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
}

// The ID hash of shaders/debugview.comp.
auto idColor(uint32_t id) -> ImU32 {
    uint32_t h = id * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    auto channel = [&](uint32_t shift) {
        return (float) ((h >> shift) & 0xFFu) / 255.0f * 0.85f + 0.15f;
    };
    return ImGui::ColorConvertFloat4ToU32(ImVec4(channel(0), channel(8), channel(16), 1.0f));
}

// A horizontal bar of the heat scale with labels under its ends and middle.
auto heatBar(bool reversed, const char* left, const char* middle, const char* right) -> void {
    auto* draw = ImGui::GetWindowDrawList();
    auto origin = ImGui::GetCursorScreenPos();
    float width = std::max(ImGui::GetContentRegionAvail().x, 100.0f);
    constexpr int steps = 64;
    for (int i = 0; i < steps; i++) {
        float t = (float) i / (float) (steps - 1);
        auto x0 = origin.x + width * (float) i / (float) steps;
        auto x1 = origin.x + width * (float) (i + 1) / (float) steps;
        draw->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, origin.y + 18.0f), heat(reversed ? 1.0f - t : t));
    }
    ImGui::Dummy(ImVec2(width, 18.0f));
    ImGui::TextUnformatted(left);
    ImGui::SameLine(width * 0.5f - ImGui::CalcTextSize(middle).x * 0.5f);
    ImGui::TextUnformatted(middle);
    ImGui::SameLine(width - ImGui::CalcTextSize(right).x);
    ImGui::TextUnformatted(right);
}

} // namespace

auto updateDebugViewCursor(DebugViewWindowState& state) -> void {
    const auto& io = ImGui::GetIO();
    auto x = io.MousePos.x * io.DisplayFramebufferScale.x;
    auto y = io.MousePos.y * io.DisplayFramebufferScale.y;
    auto width = io.DisplaySize.x * io.DisplayFramebufferScale.x;
    auto height = io.DisplaySize.y * io.DisplayFramebufferScale.y;
    state.cursorValid = !io.WantCaptureMouse && x >= 0.0f && y >= 0.0f && x < width && y < height;
    if (state.cursorValid) {
        state.cursorX = (uint32_t) x;
        state.cursorY = (uint32_t) y;
    }
}

auto debugViewReadoutWatchFor(const DebugViewWindowState& state, DebugView view) -> std::optional<CaptureWatch> {
    if (view == DebugView::None || !state.cursorValid) {
        return std::nullopt;
    }
    return CaptureWatch{
        .id = debugViewReadoutWatch,
        .pass = "DebugViewPass",
        .resource = "debugview.value",
        .live = true,
        .regionX = state.cursorX,
        .regionY = state.cursorY,
        .regionWidth = 1,
        .regionHeight = 1,
    };
}

auto drawDebugViewWindow(DebugView view, DebugViewWindowState& state, const std::function<std::string(uint32_t)>& instancePath) -> void {
    if (view == DebugView::None) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Debug View")) {
        ImGui::End();
        return;
    }
    ImGui::Text("%s", debugViewLabels[(size_t) view]);
    ImGui::Separator();
    switch (view) {
        case DebugView::Wireframe:
            ImGui::TextWrapped("Shaded surfaces with every triangle's edges; hidden edges are removed by the depth test.");
            break;
        case DebugView::TriangleSize:
            ImGui::TextWrapped("Screen area of each triangle, log scale. Red triangles cover a pixel or less: the GPU shades them in 2x2 quads, so "
                               "most of that work is wasted.");
            heatBar(true, "1 px", "~32 px", "1000+ px");
            break;
        case DebugView::Overdraw:
            ImGui::TextWrapped("Fragments shaded per pixel with no depth test: what the geometry pass would cost without early depth rejection.");
            heatBar(false, "0", "4", "8+");
            break;
        case DebugView::Uv:
            ImGui::TextWrapped("Checker of 8 cells per UV unit, red = U, green = V. Stretched or skewed cells show distorted UVs.");
            break;
        default:
            ImGui::TextWrapped("One hashed colour per ID; neighbouring IDs get unrelated colours.");
            break;
    }
    ImGui::Separator();
    if (!state.cursorValid) {
        ImGui::TextDisabled("Move the cursor over the viewport for its value.");
    } else if (!state.readout.has_value() || !state.readout->bytes || state.readout->bytes->size() < 16) {
        ImGui::TextDisabled("(%u, %u): waiting for readout", state.cursorX, state.cursorY);
    } else {
        auto v = decodeTexel(state.readout->format, state.readout->bytes->data());
        ImGui::Text("(%u, %u), frame %llu", state.readout->regionX, state.readout->regionY, (unsigned long long) state.readout->frame);
        if (v.w == 0.0f && view != DebugView::Overdraw) {
            ImGui::TextUnformatted("background");
        } else {
            auto id = (uint32_t) v.x;
            switch (view) {
                case DebugView::Wireframe:
                    ImGui::Text("%.2f px to the nearest edge", v.x);
                    break;
                case DebugView::TriangleSize:
                    ImGui::Text("triangle area %.2f px", v.x);
                    break;
                case DebugView::Overdraw:
                    ImGui::Text("%u fragments", id);
                    break;
                case DebugView::Uv:
                    ImGui::Text("u %.4f  v %.4f", v.x, v.y);
                    break;
                default:
                    ImGui::ColorButton("##id", ImGui::ColorConvertU32ToFloat4(idColor(id)), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
                    ImGui::SameLine();
                    if (view == DebugView::InstanceId) {
                        ImGui::Text("instance %u  %s", id, instancePath(id).c_str());
                    } else if (view == DebugView::MeshId) {
                        ImGui::Text("mesh table entry %u", id);
                    } else if (view == DebugView::MaterialId) {
                        ImGui::Text("material table entry %u", id);
                    } else {
                        ImGui::Text("triangle %u of its draw", id);
                    }
                    break;
            }
        }
    }
    ImGui::End();
}
