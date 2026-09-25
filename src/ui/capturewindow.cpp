#include "capturewindow.h"

#include "gpuschema.h"
#include "uiformat.h"

#include <algorithm>
#include <format>
#include <imgui.h>
#include <set>
#include <vector>

namespace {

constexpr int rowsPerPage = 256;

auto passLabel(const std::string& pass) -> const char* {
    return pass.empty() ? "(after last pass)" : pass.c_str();
}

// Resources the target pass touches, or the imported and static ones after the last pass.
auto resourcesFor(const FrameGraphDebugSnapshot& fg, const std::string& pass) -> std::vector<std::string> {
    std::set<std::string> names;
    if (pass.empty()) {
        for (const auto& r : fg.resources) {
            if (r.external) {
                names.insert(r.name);
            }
        }
        for (const auto* name : captureStaticBufferNames()) {
            names.insert(name);
        }
        return {names.begin(), names.end()};
    }
    for (const auto& p : fg.passes) {
        if (p.name != pass) {
            continue;
        }
        for (const auto& a : p.reads) {
            if (a.resourceIndex < fg.resources.size()) {
                names.insert(fg.resources[a.resourceIndex].name);
            }
        }
        for (const auto& a : p.writes) {
            if (a.resourceIndex < fg.resources.size()) {
                names.insert(fg.resources[a.resourceIndex].name);
            }
        }
    }
    return {names.begin(), names.end()};
}

auto drawTarget(const std::optional<FrameGraphDebugSnapshot>& fg, CaptureWindowState& state) -> void {
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("Pass", passLabel(state.pass))) {
        if (ImGui::Selectable("(after last pass)", state.pass.empty())) {
            state.pass.clear();
        }
        if (fg.has_value()) {
            for (auto passIdx : fg->executionOrder) {
                const auto& p = fg->passes[passIdx];
                if (ImGui::Selectable(p.name.c_str(), p.name == state.pass)) {
                    state.pass = p.name;
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("Resource", state.resource.empty() ? "(none)" : state.resource.c_str())) {
        if (fg.has_value()) {
            for (const auto& name : resourcesFor(*fg, state.pass)) {
                if (ImGui::Selectable(name.c_str(), name == state.resource)) {
                    state.resource = name;
                }
            }
        }
        ImGui::EndCombo();
    }
    if (!fg.has_value()) {
        ImGui::TextDisabled("Waiting for the frame graph snapshot...");
    }
    ImGui::BeginDisabled(state.resource.empty());
    if (ImGui::Button("Capture")) {
        state.trigger++;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Live", &state.live);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##dumppath", state.dumpPath, sizeof(state.dumpPath));
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.result.has_value() || !state.result->bytes);
    if (ImGui::Button("Dump")) {
        std::string path = state.dumpPath;
        if (state.result->texture && !path.ends_with(".png")) {
            path += ".png";
        } else if (!state.result->texture && !path.ends_with(".json")) {
            path += ".json";
        }
        state.status = writeCaptureFiles(path, *state.result, state.display, 65536) ? "wrote " + path : "cannot write " + path;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!state.status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", state.status.c_str());
    }
}

auto drawTexture(CaptureWindowState& state) -> void {
    auto& r = *state.result;
    auto& d = state.display;
    ImGui::Text("%ux%u %s, %s", r.width, r.height, toString(r.format), std::format("{} B", r.byteSize).c_str());
    ImGui::Text("min (%.4g, %.4g, %.4g, %.4g)  max (%.4g, %.4g, %.4g, %.4g)", r.channelMin.x, r.channelMin.y, r.channelMin.z, r.channelMin.w, r.channelMax.x, r.channelMax.y, r.channelMax.z, r.channelMax.w);
    const char* labels[4] = {"R", "G", "B", "A"};
    for (int c = 0; c < 4; c++) {
        if (c > 0) {
            ImGui::SameLine();
        }
        ImGui::Checkbox(labels[c], &d.channels[c]);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto range", &d.autoRange);
    if (!d.autoRange) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::DragFloatRange2("Range", &d.rangeMin, &d.rangeMax, 0.001f, -1000.0f, 1000.0f, "%.4g");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("Zoom", &state.zoom, 0.25f, 8.0f, "%.2fx");
    if (r.previewId == 0) {
        ImGui::TextDisabled("(no preview)");
        return;
    }
    ImGui::BeginChild("##image", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    // Shown at the capture's own size times zoom; the preview may be a downscaled copy.
    ImVec2 size((float) r.width * state.zoom * 0.5f, (float) r.height * state.zoom * 0.5f);
    auto origin = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID) r.previewId, size);
    if (ImGui::IsItemHovered() && r.bytes) {
        auto mouse = ImGui::GetMousePos();
        auto u = (mouse.x - origin.x) / size.x;
        auto v = (mouse.y - origin.y) / size.y;
        auto x = std::clamp((uint32_t) (u * (float) r.width), 0u, r.width - 1);
        auto y = std::clamp((uint32_t) (v * (float) r.height), 0u, r.height - 1);
        auto texel = captureTexelBytes(r.format);
        auto value = decodeTexel(r.format, r.bytes->data() + (((uint64_t) y * r.width + x) * texel));
        ImGui::SetTooltip("(%u, %u)\n%.6g  %.6g  %.6g  %.6g", x, y, value.x, value.y, value.z, value.w);
    }
    ImGui::EndChild();
}

auto drawBuffer(CaptureWindowState& state) -> void {
    auto& r = *state.result;
    const auto& schema = schemaForResource(r.resource);
    auto rows = (r.bytes && schema.stride > 0) ? (int) (r.bytes->size() / schema.stride) : 0;
    char buf[32];
    ImGui::Text("%s, %s: %d rows of %s (%u B)", r.resource.c_str(), formatByteCount(r.byteSize, buf, sizeof(buf)), rows, schema.name.c_str(), schema.stride);
    auto pages = std::max(1, (rows + rowsPerPage - 1) / rowsPerPage);
    state.page = std::clamp(state.page, 0, pages - 1);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Page", &state.page, 0, pages - 1);
    ImGui::SameLine();
    ImGui::Checkbox("Hex", &state.hex);
    auto first = state.page * rowsPerPage;
    auto last = std::min(rows, first + rowsPerPage);
    if (state.hex) {
        ImGui::BeginChild("##hex", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
        for (int row = first; row < last; row++) {
            std::string line = std::format("{:6} ", row);
            for (uint32_t b = 0; b < schema.stride; b++) {
                line += std::format("{:02x}", std::to_integer<uint8_t>((*r.bytes)[(size_t) row * schema.stride + b]));
                if ((b % 4) == 3) {
                    line += ' ';
                }
            }
            ImGui::TextUnformatted(line.c_str());
        }
        ImGui::EndChild();
        return;
    }
    auto columns = (int) std::min<size_t>(schema.fields.size() + 1, 64);
    auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##rows", columns, flags)) {
        ImGui::TableSetupColumn("row", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        for (int c = 1; c < columns; c++) {
            ImGui::TableSetupColumn(schema.fields[c - 1].name.c_str());
        }
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();
        for (int row = first; row < last; row++) {
            auto values = decodeRow(schema, *r.bytes, row);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", row);
            for (int c = 1; c < columns; c++) {
                ImGui::TableSetColumnIndex(c);
                ImGui::TextUnformatted(values[c - 1].c_str());
            }
        }
        ImGui::EndTable();
    }
}

} // namespace

auto captureWindowWatch(const CaptureWindowState& state) -> std::optional<CaptureWatch> {
    if (state.resource.empty()) {
        return std::nullopt;
    }
    return CaptureWatch{.id = captureWindowWatchId, .pass = state.pass, .resource = state.resource, .live = state.live, .trigger = state.trigger, .display = state.display};
}

auto drawCaptureWindow(bool& show, const std::optional<FrameGraphDebugSnapshot>& fg, CaptureWindowState& state) -> void {
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Capture", &show)) {
        ImGui::End();
        return;
    }
    drawTarget(fg, state);
    ImGui::Separator();
    if (!state.result.has_value()) {
        ImGui::TextDisabled("Pick a pass and resource, then Capture.");
    } else {
        const auto& r = *state.result;
        ImGui::Text("Frame %llu: %s after %s", (unsigned long long) r.frame, r.resource.c_str(), passLabel(r.pass));
        if (!r.error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", r.error.c_str());
        } else if (r.texture) {
            drawTexture(state);
        } else {
            drawBuffer(state);
        }
    }
    ImGui::End();
}
