#include "framegraphwindow.h"

#include "framegraphnodeview.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <utility>

namespace {

auto accessColor(FgAccessFlags a) -> ImVec4 {
    switch (std::to_underlying(a)) {
        case std::to_underlying(FgAccessFlags::ColorAttachment):
            return {0.20f, 0.60f, 0.30f, 1.0f};
        case std::to_underlying(FgAccessFlags::DepthAttachment):
            return {0.55f, 0.35f, 0.15f, 1.0f};
        case std::to_underlying(FgAccessFlags::ShaderRead):
            return {0.20f, 0.40f, 0.75f, 1.0f};
        case std::to_underlying(FgAccessFlags::TransferSrc):
            return {0.60f, 0.40f, 0.10f, 1.0f};
        case std::to_underlying(FgAccessFlags::TransferDst):
            return {0.60f, 0.40f, 0.10f, 1.0f};
        case std::to_underlying(FgAccessFlags::Present):
            return {0.50f, 0.20f, 0.55f, 1.0f};
        default:
            return {0.40f, 0.40f, 0.40f, 1.0f};
    }
}

void accessChip(FgAccessFlags a) {
    auto color = accessColor(a);
    const char* text = toString(a);
    auto textSize = ImGui::CalcTextSize(text);
    auto padding = ImGui::GetStyle().FramePadding;
    ImVec2 chipSize(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
    auto pos = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    auto bg = ImGui::GetColorU32(color);
    dl->AddRectFilled(pos, ImVec2(pos.x + chipSize.x, pos.y + chipSize.y), bg, 3.0f);
    dl->AddText(ImVec2(pos.x + padding.x, pos.y + padding.y), IM_COL32_WHITE, text);
    ImGui::Dummy(chipSize);
}

void drawPassDetail(const FrameGraphDebugSnapshot& snap, uint32_t passIdx, std::optional<uint32_t>& selResource) {
    if (passIdx >= snap.passes.size()) {
        ImGui::TextUnformatted("(invalid pass)");
        return;
    }
    const auto& pass = snap.passes[passIdx];
    ImGui::TextUnformatted(pass.name.c_str());
    ImGui::Separator();

    if (pass.executionIndex != UINT32_MAX) {
        ImGui::Text("Execution index: %u", pass.executionIndex);
    } else {
        ImGui::TextDisabled("Not scheduled");
    }
    ImGui::Text("Culled: %s", pass.culled ? "yes" : "no");
    ImGui::Text("Side effects: %s", pass.hasSideEffects ? "yes" : "no");
    ImGui::Spacing();

    auto drawAccessTable = [&](const char* label, const std::vector<FgResourceAccessDebug>& list) {
        ImGui::TextUnformatted(label);
        if (list.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled(" (none)");
            return;
        }
        if (ImGui::BeginTable(label, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 72.0f);
            ImGui::TableSetupColumn("Resource");
            ImGui::TableSetupColumn("Access", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            for (size_t rowIndex = 0; rowIndex < list.size(); rowIndex++) {
                const auto& a = list[rowIndex];
                ImGui::TableNextRow();
                ImGui::PushID((int) (a.resourceIndex * 31 + rowIndex));

                const auto* res = a.resourceIndex < snap.resources.size() ? &snap.resources[a.resourceIndex] : nullptr;

                // Invisible Selectable first, spanning the whole row, so later widgets can draw on top.
                ImGui::TableSetColumnIndex(0);
                bool selected = selResource == a.resourceIndex;
                if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 48.0f))) {
                    selResource = a.resourceIndex;
                }

                // Now draw content on top.
                ImGui::SameLine();
                if (res != nullptr && res->previewTextureId != 0 && res->previewWidth > 0 && res->previewHeight > 0) {
                    auto aspect = (float) res->previewWidth / (float) res->previewHeight;
                    float h = 48.0f;
                    float w = h * aspect;
                    ImGui::Image((ImTextureID) res->previewTextureId, ImVec2(w, h));
                } else {
                    ImGui::Dummy(ImVec2(64, 48));
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(res != nullptr ? res->label.c_str() : "(invalid)");

                ImGui::TableSetColumnIndex(2);
                accessChip(a.access);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    };

    drawAccessTable("Reads", pass.reads);
    ImGui::Spacing();
    drawAccessTable("Writes", pass.writes);
}

void drawResourceDetail(const FrameGraphDebugSnapshot& snap, uint32_t resIdx, std::optional<uint32_t>& selPass) {
    if (resIdx >= snap.resources.size()) {
        ImGui::TextUnformatted("(invalid resource)");
        return;
    }
    const auto& res = snap.resources[resIdx];
    ImGui::TextUnformatted(res.label.c_str());
    ImGui::Separator();

    ImGui::Text("Size: %u x %u", res.width, res.height);
    ImGui::Text("Format: %s", res.formatName);
    ImGui::Text("Usage: %s", res.usageName);
    ImGui::Text("External: %s", res.external ? "yes" : "no");
    if (res.external) {
        ImGui::TextDisabled("Lifetime not tracked for imported resources");
    } else if (res.firstUseOrder == UINT32_MAX) {
        ImGui::TextDisabled("Unused");
    } else {
        ImGui::Text("Lifetime: exec %u .. %u", res.firstUseOrder, res.lastUseOrder);
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Preview:");
    if (res.previewTextureId != 0 && res.previewWidth > 0 && res.previewHeight > 0) {
        auto aspect = (float) res.previewWidth / (float) res.previewHeight;
        float displayW = std::min(320.0f, ImGui::GetContentRegionAvail().x);
        float displayH = displayW / aspect;
        ImGui::Image((ImTextureID) res.previewTextureId, ImVec2(displayW, displayH));
    } else {
        ImGui::TextDisabled("(no preview — format not blittable or debug just enabled)");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Producer:");
    ImGui::SameLine();
    if (res.producerPass == UINT32_MAX) {
        ImGui::TextDisabled("(imported / none)");
    } else {
        const auto& prod = snap.passes[res.producerPass];
        ImGui::PushID((int) res.producerPass);
        if (ImGui::SmallButton(prod.name.c_str())) {
            selPass = res.producerPass;
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Consumers:");
    if (res.consumerPasses.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled(" (none)");
    } else {
        for (auto c : res.consumerPasses) {
            ImGui::PushID((int) c);
            if (ImGui::SmallButton(snap.passes[c].name.c_str())) {
                selPass = c;
            }
            ImGui::PopID();
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }
}

} // namespace

void drawFrameGraphWindow(bool& show,
                          const std::optional<FrameGraphDebugSnapshot>& snap,
                          std::optional<uint32_t>& selPass,
                          std::optional<uint32_t>& selResource) {
    if (!show) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(720, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Frame Graph", &show)) {
        ImGui::End();
        return;
    }

    if (!snap.has_value()) {
        ImGui::TextDisabled("Waiting for render thread snapshot...");
        ImGui::End();
        return;
    }

    const auto& s = *snap;
    uint32_t culledCount = 0;
    uint32_t previewCount = 0;
    for (const auto& p : s.passes) {
        if (p.culled) {
            culledCount++;
        }
    }
    for (const auto& r : s.resources) {
        if (r.previewTextureId != 0) {
            previewCount++;
        }
    }
    ImGui::Text("Frame #%llu — %zu passes (%u culled), %zu resources, %u previews",
                (unsigned long long) s.frameIndex,
                s.passes.size(),
                culledCount,
                s.resources.size(),
                previewCount);
    ImGui::Separator();

    auto drawDetailPanes = [&]() {
        float bottomHeight = std::min(420.0f, ImGui::GetContentRegionAvail().y * 0.55f);

        ImGui::BeginChild("##fg_pass_detail", ImVec2(0, -bottomHeight), ImGuiChildFlags_Borders);
        if (selPass.has_value()) {
            drawPassDetail(s, *selPass, selResource);
        } else {
            ImGui::TextDisabled("Select a pass to inspect its reads and writes.");
        }
        ImGui::EndChild();

        ImGui::BeginChild("##fg_resource_detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
        if (selResource.has_value()) {
            drawResourceDetail(s, *selResource, selPass);
        } else {
            ImGui::TextDisabled("Click a resource to see its details and preview here.");
        }
        ImGui::EndChild();
    };

    if (ImGui::BeginTabBar("##fg_tabs")) {
        if (ImGui::BeginTabItem("List")) {
            if (ImGui::BeginTable("##fg_split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("Passes", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                if (ImGui::BeginTable(
                        "##fg_passes", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
                    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.0f);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthFixed, 28.0f);
                    ImGui::TableSetupColumn("W", ImGuiTableColumnFlags_WidthFixed, 28.0f);
                    ImGui::TableHeadersRow();

                    for (uint32_t i = 0; i < s.executionOrder.size(); i++) {
                        auto passIdx = s.executionOrder[i];
                        const auto& pass = s.passes[passIdx];
                        ImGui::TableNextRow();
                        ImGui::PushID((int) passIdx);

                        bool selected = selPass == passIdx;
                        if (pass.culled) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        }

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%u", i);
                        ImGui::TableSetColumnIndex(1);
                        if (ImGui::Selectable(pass.name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                            selPass = passIdx;
                        }
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%zu", pass.reads.size());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::Text("%zu", pass.writes.size());

                        if (pass.culled) {
                            ImGui::PopStyleColor();
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }

                ImGui::TableSetColumnIndex(1);
                drawDetailPanes();

                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Graph")) {
            if (ImGui::BeginTable("##fg_graph_split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("Canvas", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                drawFrameGraphNodeView(s, selPass, selResource);

                ImGui::TableSetColumnIndex(1);
                drawDetailPanes();

                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}
