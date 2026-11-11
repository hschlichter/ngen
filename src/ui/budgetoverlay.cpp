#include "budgetoverlay.h"

#include "axis3dgizmo.h"
#include "profile.h"
#include "profileconfig.h"
#include "scenegpustats.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

// The frames the bars average over: the zones record's window.
constexpr uint32_t averageFrames = 60;

constexpr float labelGap = 10.0f;
constexpr float barWidth = 180.0f;
constexpr float valueWidth = 64.0f;
constexpr float groupBarHeight = 12.0f;
constexpr float passBarHeight = 6.0f;
constexpr float passIndent = 12.0f;

struct GroupRow {
    profile::Group group = profile::Group::Render;
    double ms = 0.0;
    std::vector<profile::GpuPassTime> passes;
};

// A group in the words of someone making content, not the engine's.
auto groupLabel(profile::Group group) -> const char* {
    switch (group) {
        case profile::Group::Shadow:
            return "Shadows";
        case profile::Group::Geometry:
            return "Geometry";
        case profile::Group::Lighting:
            return "Lighting";
        case profile::Group::Post:
            return "Post-processing";
        case profile::Group::Culling:
            return "Culling";
        case profile::Group::Ui:
            return "UI";
        case profile::Group::Tools:
            return "Editor tools";
        case profile::Group::Upload:
            return "Uploads";
        case profile::Group::Render:
            return "Other rendering";
        default:
            return profile::groupName(group);
    }
}

auto rgbToImGui(uint32_t rgb, uint8_t alpha) -> ImU32 {
    return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, alpha);
}

// Over the budget means more than 5% over it: frame times jitter by a fraction of a millisecond, and a frame locked to the display's rate sits
// right on a budget of that rate (16.6 and 16.7 ms at 60 Hz), which would otherwise flip between on and over every frame.
constexpr double overBudget = 1.05;

// Green under 80% of the budget, yellow up to it (and within 5% over), red over.
auto budgetColor(double ms, double budgetMs) -> ImU32 {
    if (ms < budgetMs * 0.8) {
        return IM_COL32(90, 200, 90, 255);
    }
    if (ms <= budgetMs * overBudget) {
        return IM_COL32(230, 190, 60, 255);
    }
    return IM_COL32(230, 70, 60, 255);
}

auto formatMs(double ms, char* out, size_t size) -> const char* {
    if (ms < 1.0) {
        std::snprintf(out, size, "%.2f ms", ms);
    } else {
        std::snprintf(out, size, "%.1f ms", ms);
    }
    return out;
}

auto formatCount(uint64_t count, char* out, size_t size) -> const char* {
    if (count >= 1'000'000) {
        std::snprintf(out, size, "%.1fM", (double) count / 1'000'000.0);
    } else if (count >= 1'000) {
        std::snprintf(out, size, "%.1fK", (double) count / 1'000.0);
    } else {
        std::snprintf(out, size, "%llu", (unsigned long long) count);
    }
    return out;
}

// One row: a label in a column `labelWidth` wide, a bar of `ms` against the budget (the bar's full width), the milliseconds. Over the budget
// (overBudget), the bar stops at the edge with a red cap.
auto barRow(const char* label, float labelWidth, double ms, double budgetMs, ImU32 color, float height, float indent, bool dim) -> void {
    auto* draw = ImGui::GetWindowDrawList();
    auto origin = ImGui::GetCursorScreenPos();
    auto lineHeight = ImGui::GetTextLineHeight();
    auto textColor = dim ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : ImGui::GetColorU32(ImGuiCol_Text);
    draw->AddText(ImVec2(origin.x + indent, origin.y), textColor, label);

    float barX = origin.x + labelWidth;
    float barY = origin.y + ((lineHeight - height) * 0.5f);
    draw->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barWidth, barY + height), IM_COL32(255, 255, 255, 25));
    auto share = budgetMs > 0.0 ? ms / budgetMs : 0.0;
    float filled = barWidth * (float) std::clamp(share, 0.0, 1.0);
    draw->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + filled, barY + height), color);
    if (share > overBudget) {
        draw->AddRectFilled(ImVec2(barX + barWidth - 3.0f, barY - 1.0f), ImVec2(barX + barWidth, barY + height + 1.0f), IM_COL32(230, 70, 60, 255));
    }

    char value[32];
    formatMs(ms, value, sizeof(value));
    auto valueSize = ImGui::CalcTextSize(value);
    draw->AddText(ImVec2(barX + barWidth + valueWidth - valueSize.x, origin.y), textColor, value);
    ImGui::Dummy(ImVec2(labelWidth + barWidth + valueWidth, lineHeight));
}

} // namespace

auto drawBudgetOverlay(const RenderFrameCommands& commands) -> void {
    thread_local profile::GpuPassTimes times;
    thread_local std::vector<GroupRow> groups;
    profile::gpuPassTimes(averageFrames, times);
    // Groups in the order the frame runs their first pass.
    groups.clear();
    for (const auto& pass : times.passes) {
        auto group = profile::groupOf(pass.siteId);
        auto row = std::ranges::find_if(groups, [group](const GroupRow& r) { return r.group == group; });
        if (row == groups.end()) {
            groups.push_back({.group = group});
            row = groups.end() - 1;
        }
        row->ms += pass.msPerFrame;
        row->passes.push_back(pass);
    }
    double budgetMs = profileconfig::frameBudgetMs.get();

    const auto* viewport = ImGui::GetMainViewport();
    // First under the corner axis gizmo, which takes the viewport's top-right corner (in pixels from the window's top, as the gizmo is
    // placed); dragged anywhere on it, it stays where it is put, kept in imgui.ini like the windows.
    constexpr float margin = 10.0f;
    float top = viewport->Pos.y + (float) (Axis3DGizmo::margin + Axis3DGizmo::gizmoSize) + margin;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - margin, top), ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin("##FrameBudget", nullptr, flags)) {
        ImGui::End();
        return;
    }
    // The label column fits the longest label: pass names run long.
    float labelWidth = ImGui::CalcTextSize("Whole frame").x;
    for (const auto& row : groups) {
        labelWidth = std::max(labelWidth, ImGui::CalcTextSize(groupLabel(row.group)).x);
        for (const auto& pass : row.passes) {
            labelWidth = std::max(labelWidth, passIndent + ImGui::CalcTextSize(profile::nameOf(pass.siteId)).x);
        }
    }
    labelWidth += labelGap;
    char budget[32];
    ImGui::Text("Frame budget %s (%.0f fps)", formatMs(budgetMs, budget, sizeof(budget)), 1000.0 / budgetMs);
    if (times.frames == 0 || times.passes.empty()) {
        ImGui::TextDisabled("Waiting for GPU timings");
        ImGui::End();
        return;
    }
    barRow("Whole frame", labelWidth, times.frameMs, budgetMs, budgetColor(times.frameMs, budgetMs), groupBarHeight, 0.0f, false);
    barRow("GPU", labelWidth, times.gpuFrameMs, budgetMs, budgetColor(times.gpuFrameMs, budgetMs), groupBarHeight, 0.0f, false);
    ImGui::Separator();
    for (const auto& row : groups) {
        auto groupRgb = profile::groupColor(row.group, 0);
        barRow(groupLabel(row.group), labelWidth, row.ms, budgetMs, rgbToImGui(groupRgb, 230), groupBarHeight, 0.0f, false);
        for (const auto& pass : row.passes) {
            barRow(profile::nameOf(pass.siteId), labelWidth, pass.msPerFrame, budgetMs, rgbToImGui(groupRgb, 140), passBarHeight, passIndent, true);
        }
    }
    ImGui::Separator();
    char draws[32];
    char triangles[32];
    formatCount(commands.draws, draws, sizeof(draws));
    formatCount(commands.primitives, triangles, sizeof(triangles));
    ImGui::Text("%s draws, %s triangles (all passes)", draws, triangles);
    ImGui::End();
}

namespace uiconfig {

config::Bool budgetOverlay{"ui.overlay.budget",
                           false,
                           "The frame budget overlay: the GPU's passes by group as bars against profile.budget.frameMs, averaged over 60 frames "
                           "(Ctrl+B)."};

} // namespace uiconfig
