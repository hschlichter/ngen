#include "framedebuggerwindow.h"

#include "drawlists.h"
#include "gpuschema.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <imgui.h>

namespace {

// The pass before `order` in execution order, or "" when it is the first.
auto previousPass(const FrameDebugCapture& c, uint32_t order) -> std::string {
    return order == 0 ? std::string() : c.passes[order - 1].name;
}

auto drawOverview(const FrameDebugPass& pass) -> void {
    ImGui::Text("Pass %u: %s%s", pass.order, pass.name.c_str(), pass.culled ? " (culled)" : "");
    if (pass.culled) {
        ImGui::TextDisabled("Culled: no pass reads its outputs and it has no side effects, so it was not recorded.");
    }
    ImGui::Text("GPU %.3f ms   draws %u   indirect calls %u   dispatches %u   barriers %u   primitives %llu", pass.gpuMs, pass.stats.draws, pass.stats.indirectDraws, pass.stats.dispatches, pass.stats.barriers, (unsigned long long) pass.stats.primitives);
    ImGui::Text("pipeline binds %u   descriptor binds %u   buffer binds %u   copies %u", pass.stats.pipelineBinds, pass.stats.descriptorBinds, pass.stats.bufferBinds, pass.stats.copies);
    ImGui::SeparatorText("Reads");
    for (const auto& a : pass.reads) {
        ImGui::BulletText("%s  (%s)", a.resource.c_str(), a.access.c_str());
    }
    ImGui::SeparatorText("Writes");
    for (const auto& a : pass.writes) {
        ImGui::BulletText("%s  (%s)", a.resource.c_str(), a.access.c_str());
    }
}

auto drawBarriers(const FrameDebugPass& pass) -> void {
    if (pass.barriers.empty()) {
        ImGui::TextDisabled("No barriers: every resource was already in the state this pass needs.");
        return;
    }
    ImGui::TextWrapped("The graph derives each barrier from the resource's previous access and the one this pass declares; the backend turns the "
                       "RHI states into Vulkan stages, accesses and layouts.");
    auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX;
    if (ImGui::BeginTable("##barriers", 6, flags)) {
        ImGui::TableSetupColumn("Resource");
        ImGui::TableSetupColumn("Access");
        ImGui::TableSetupColumn("RHI state");
        ImGui::TableSetupColumn("Vulkan stages");
        ImGui::TableSetupColumn("Vulkan access");
        ImGui::TableSetupColumn("Layout");
        ImGui::TableHeadersRow();
        for (const auto& b : pass.barriers) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s (%s)", b.resource.c_str(), b.buffer ? "buffer" : "texture");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s -> %s", b.oldAccess.c_str(), b.newAccess.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s -> %s", b.oldState.c_str(), b.newState.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextWrapped("%s\n-> %s", b.backend.srcStages.c_str(), b.backend.dstStages.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextWrapped("%s\n-> %s", b.backend.srcAccess.c_str(), b.backend.dstAccess.c_str());
            ImGui::TableSetColumnIndex(5);
            if (b.buffer) {
                ImGui::TextDisabled("(buffers have no layout)");
            } else {
                ImGui::TextWrapped("%s\n-> %s", b.backend.oldLayout.c_str(), b.backend.newLayout.c_str());
            }
        }
        ImGui::EndTable();
    }
}

auto drawCommands(const FrameDebugPass& pass, FrameDebuggerState& state) -> void {
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputText("Filter", state.commandFilter, sizeof(state.commandFilter));
    ImGui::BeginChild("##commands", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (size_t i = 0; i < pass.commands.size(); i++) {
        const auto& text = pass.commands[i];
        if (state.commandFilter[0] != '\0' && text.find(state.commandFilter) == std::string::npos) {
            continue;
        }
        ImGui::Text("%4zu  %s", i, text.c_str());
    }
    ImGui::EndChild();
}

auto drawDescriptors(const FrameDebugPass& pass, const FrameDebugCapture& capture) -> void {
    if (pass.descriptorSets.empty()) {
        ImGui::TextDisabled("This pass bound no descriptor sets.");
        return;
    }
    for (const auto& name : pass.descriptorSets) {
        auto it = std::ranges::find_if(capture.sets, [&](const FrameDebugSet& s) { return s.name == name; });
        if (!ImGui::TreeNode(name.c_str())) {
            continue;
        }
        if (it != capture.sets.end()) {
            // Array bindings with many elements (the texture array) collapse repeated resources.
            for (size_t i = 0; i < it->descriptors.size(); i++) {
                const auto& d = it->descriptors[i];
                size_t run = i;
                while (run + 1 < it->descriptors.size() && it->descriptors[run + 1].binding == d.binding && it->descriptors[run + 1].resource == d.resource) {
                    run++;
                }
                if (run > i) {
                    ImGui::BulletText("binding %u [%u..%u]: %s", d.binding, d.arrayElement, it->descriptors[run].arrayElement, d.resource.c_str());
                    i = run;
                } else {
                    ImGui::BulletText("binding %u [%u]: %s", d.binding, d.arrayElement, d.resource.c_str());
                }
            }
        }
        ImGui::TreePop();
    }
}

auto drawResultThumb(const CaptureResult* r, const char* label, std::function<void(const std::string&, const std::string&)>& openInCapture) -> void {
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label);
    if (r == nullptr) {
        ImGui::TextDisabled("(capturing...)");
    } else if (!r->error.empty()) {
        ImGui::TextDisabled("%s", r->error.c_str());
    } else if (r->texture && r->previewId != 0) {
        float w = 240.0f;
        float h = w * (float) r->previewHeight / (float) std::max(1u, r->previewWidth);
        ImGui::Image((ImTextureID) r->previewId, ImVec2(w, h));
        ImGui::TextDisabled("frame %llu", (unsigned long long) r->frame);
    } else {
        const auto& schema = schemaForResource(r->resource);
        auto rows = r->bytes && schema.stride > 0 ? r->bytes->size() / schema.stride : 0;
        ImGui::Text("%zu rows of %s", rows, schema.name.c_str());
        ImGui::TextDisabled("frame %llu", (unsigned long long) r->frame);
    }
    if (r != nullptr && ImGui::SmallButton(std::format("Open##{}{}", label, r->id).c_str())) {
        openInCapture(r->pass, r->resource);
    }
    ImGui::EndGroup();
}

auto drawResources(const FrameDebugPass& pass, FrameDebuggerState& state, std::function<void(const std::string&, const std::string&)>& openInCapture) -> void {
    state.watchResources = true;
    for (size_t i = 0; i < pass.writes.size(); i++) {
        const auto& w = pass.writes[i];
        ImGui::SeparatorText(std::format("{} ({})", w.resource, w.access).c_str());
        auto before = state.results.find(frameDebuggerWatchBase + (uint32_t) (2 * i));
        auto after = state.results.find(frameDebuggerWatchBase + (uint32_t) (2 * i) + 1);
        drawResultThumb(before != state.results.end() ? &before->second : nullptr, "before", openInCapture);
        ImGui::SameLine();
        drawResultThumb(after != state.results.end() ? &after->second : nullptr, "after", openInCapture);
    }
}

auto regionName(uint32_t region) -> std::string {
    auto view = region / DrawLists::bucketCount;
    auto bucket = region % DrawLists::bucketCount == 0 ? "single-sided" : "double-sided";
    return view == 0 ? std::format("camera, {}", bucket) : std::format("cascade {}, {}", view - 1, bucket);
}

auto drawDraws(const FrameDebugPass& pass, FrameDebuggerState& state, const std::function<std::string(uint32_t)>& primPathOfInstance) -> void {
    bool indirect = std::ranges::any_of(pass.reads, [](const FrameDebugAccess& a) { return a.resource == "drawCommands"; });
    if (!indirect) {
        ImGui::TextDisabled("Not an indirect pass: it reads no draw commands.");
        return;
    }
    state.watchDraws = true;
    auto commands = state.results.find(frameDebuggerCommandsWatch);
    auto counts = state.results.find(frameDebuggerCountsWatch);
    if (commands == state.results.end() || counts == state.results.end() || !commands->second.bytes || !counts->second.bytes) {
        ImGui::TextDisabled("Capturing draw commands...");
        return;
    }
    ImGui::TextDisabled("Draw commands and counts captured on frame %llu.", (unsigned long long) commands->second.frame);
    bool camera = pass.name != "ShadowPass";
    uint32_t firstRegion = camera ? 0 : DrawLists::bucketCount;
    uint32_t lastRegion = camera ? DrawLists::bucketCount : DrawLists::regionCount;
    const auto& bytes = *commands->second.bytes;
    auto capacity = (uint32_t) (bytes.size() / sizeof(RhiDrawIndexedIndirectCommand) / DrawLists::regionCount);
    for (uint32_t region = firstRegion; region < lastRegion; region++) {
        uint32_t count = 0;
        std::memcpy(&count, counts->second.bytes->data() + (DrawLists::countOffset(region)), sizeof(uint32_t));
        if (!ImGui::TreeNode(std::format("region {} ({}): {} draws", region, regionName(region), count).c_str())) {
            continue;
        }
        auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##draws", 6, flags, ImVec2(0.0f, 260.0f))) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("Prim");
            ImGui::TableSetupColumn("Instance", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("firstIndex", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("vertexOffset", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableHeadersRow();
            for (uint32_t i = 0; i < count && i < capacity; i++) {
                RhiDrawIndexedIndirectCommand command;
                std::memcpy(&command, bytes.data() + (((size_t) region * capacity + i) * sizeof(command)), sizeof(command));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%u", i);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(primPathOfInstance(command.firstInstance).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", command.firstInstance);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%u", command.indexCount / 3);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%u", command.firstIndex);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%d", command.vertexOffset);
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}

} // namespace

auto frameDebuggerWatches(const FrameDebuggerState& state) -> std::vector<CaptureWatch> {
    std::vector<CaptureWatch> watches;
    if (!state.capture.has_value() || state.selectedPass < 0 || state.selectedPass >= (int) state.capture->passes.size()) {
        return watches;
    }
    const auto& pass = state.capture->passes[state.selectedPass];
    if (state.watchResources) {
        for (size_t i = 0; i < pass.writes.size(); i++) {
            auto before = previousPass(*state.capture, pass.order);
            if (!before.empty()) {
                watches.push_back({.id = frameDebuggerWatchBase + (uint32_t) (2 * i), .pass = before, .resource = pass.writes[i].resource, .trigger = state.trigger});
            }
            watches.push_back({.id = frameDebuggerWatchBase + (uint32_t) (2 * i) + 1, .pass = pass.name, .resource = pass.writes[i].resource, .trigger = state.trigger});
        }
    }
    if (state.watchDraws) {
        watches.push_back({.id = frameDebuggerCommandsWatch, .pass = "InstanceCullScatter", .resource = "drawCommands", .trigger = state.trigger});
        watches.push_back({.id = frameDebuggerCountsWatch, .pass = "InstanceCullScan", .resource = "drawCounts", .trigger = state.trigger});
    }
    return watches;
}

auto drawFrameDebuggerWindow(bool& show, FrameDebuggerState& state, const std::function<std::string(uint32_t)>& primPathOfInstance, std::function<void(const std::string&, const std::string&)> openInCapture) -> void {
    ImGui::SetNextWindowSize(ImVec2(980, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Frame Debugger", &show)) {
        state.watchResources = false;
        state.watchDraws = false;
        ImGui::End();
        return;
    }
    if (ImGui::Button("Capture frame")) {
        state.requestCapture = true;
    }
    ImGui::SameLine();
    if (state.capture.has_value()) {
        ImGui::Text("Frame %llu: %zu passes, %zu descriptor sets", (unsigned long long) state.capture->frame, state.capture->passes.size(), state.capture->sets.size());
    } else {
        ImGui::TextDisabled("No frame captured yet.");
        ImGui::End();
        return;
    }
    const auto& c = *state.capture;
    ImGui::BeginChild("##passes", ImVec2(220.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    for (const auto& pass : c.passes) {
        auto label = std::format("{:2} {}", pass.order, pass.name);
        if (pass.culled) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        }
        if (ImGui::Selectable(label.c_str(), state.selectedPass == (int) pass.order)) {
            if (state.selectedPass != (int) pass.order) {
                state.selectedPass = (int) pass.order;
                state.results.clear();
                state.trigger++;
            }
        }
        if (pass.culled) {
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##detail");
    bool resourcesVisible = false;
    bool drawsVisible = false;
    if (state.selectedPass >= 0 && state.selectedPass < (int) c.passes.size()) {
        const auto& pass = c.passes[state.selectedPass];
        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("Overview")) {
                drawOverview(pass);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(std::format("Barriers ({})###barriers", pass.barriers.size()).c_str())) {
                drawBarriers(pass);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(std::format("Commands ({})###commands", pass.commands.size()).c_str())) {
                drawCommands(pass, state);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Descriptors")) {
                drawDescriptors(pass, c);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Resources")) {
                resourcesVisible = true;
                drawResources(pass, state, openInCapture);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Draws")) {
                drawsVisible = true;
                drawDraws(pass, state, primPathOfInstance);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    } else {
        ImGui::TextDisabled("Select a pass.");
    }
    state.watchResources = resourcesVisible;
    state.watchDraws = drawsVisible;
    ImGui::EndChild();
    ImGui::End();
}
