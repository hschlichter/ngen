#include "sceneflowwindow.h"

#include "enginemap.h"

#include <imgui.h>

#include <algorithm>
#include <format>
#include <string>

namespace {

auto mib(uint64_t bytes) -> double {
    return (double) bytes / (1024.0 * 1024.0);
}

// Dimmed text wrapped at the window's edge: what a stage or a part of it is.
auto note(const char* text) -> void {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// A stage: its name, then what it is and where its data goes.
auto stage(const char* name, const char* what) -> bool {
    bool open = ImGui::CollapsingHeader(name, ImGuiTreeNodeFlags_DefaultOpen);
    if (open) {
        note(what);
    }
    return open;
}

auto arrow(const char* what) -> void {
    ImGui::Indent(16.0f);
    note((std::string("-> ") + what).c_str());
    ImGui::Unindent(16.0f);
}

// A bar of used against capacity, with the limit the capacity may grow to.
auto poolBar(const char* name, uint64_t used, uint64_t capacity, uint64_t limit) -> void {
    auto fraction = limit > 0 ? (float) ((double) used / (double) limit) : 0.0f;
    auto label = std::format("{}: {} of {} allocated, limit {}", name, used, capacity, limit);
    ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), ImVec2(-FLT_MIN, 0.0f), label.c_str());
}

auto drawAssets(const SceneFlowWindowState& state) -> void {
    const auto& a = state.assets;
    if (!stage("1. Asset client", "Requests packed assets from ngen-asset-server and takes its answers; the scene thread asks it and is woken by it.")) {
        return;
    }
    ImGui::Text("requested %llu, received %llu (packed %llu, from the server's cache %llu), failed %llu, in flight %llu",
                (unsigned long long) a.requested,
                (unsigned long long) a.received,
                (unsigned long long) a.packed,
                (unsigned long long) a.cached,
                (unsigned long long) a.failed,
                (unsigned long long) a.inFlight());
    ImGui::Text("%.1f MiB received", mib(a.bytes));
}

auto drawThread(const SceneFlowWindowState& state) -> void {
    const auto& t = state.thread;
    if (!stage("2. Scene thread", "Composes the World from the layer packs (as USD composes a stage), extracts the draws, and keeps meshes, materials and textures by asset.")) {
        return;
    }
    ImGui::Text("requests: %llu posted, %llu applied; %llu batches", (unsigned long long) t.requestsPosted, (unsigned long long) t.requestsApplied, (unsigned long long) t.batches);
    ImGui::Text("compose: %llu, last %.1f ms%s", (unsigned long long) t.composes, t.lastComposeMs, t.composeNeeded ? " (needed)" : "");
    ImGui::Text("extract: %llu, last %.1f ms%s", (unsigned long long) t.extracts, t.lastExtractMs, t.extractNeeded ? " (needed)" : "");
    ImGui::Text("in flight: %u layers, %u meshes, %u materials, %u textures, %u other", t.inFlightLayers, t.inFlightMeshes, t.inFlightMaterials, t.inFlightTextures, t.inFlightOther);
    ImGui::Text("held: %u layer packs, %u mesh packs, %u material packs, %u texture packs; %u failed", t.layerPacks, t.meshPacks, t.materialPacks, t.texturePacks, t.failedAssets);
    ImGui::Text("libraries (told to the renderer): %u meshes, %u materials, %u textures", t.meshEntries, t.materialEntries, t.textureEntries);
    ImGui::Text("%s, %.0f ms since the scene was opened", t.idle ? "idle" : "working", t.sinceOpenMs);
}

auto drawGpu(SceneFlowWindowState& state) -> void {
    const auto& g = state.gpu;
    if (g.uploads.budget != state.shownBudget) {
        state.shownBudget = g.uploads.budget;
        state.uploadBudgetMiB = (int) (g.uploads.budget / (1024 * 1024));
    }
    if (g.geometryPool.limitBytes != state.shownLimit) {
        state.shownLimit = g.geometryPool.limitBytes;
        state.geometryLimitMiB = (int) (g.geometryPool.limitBytes / (1024 * 1024));
    }
    if (!stage("4. GPU scene (render thread)", "Applies the change lists to the GPU tables. Data goes through the upload queue, a budget per frame, into the geometry pool, the tables and textures.")) {
        return;
    }
    ImGui::Text("frame %llu; change lists %llu submitted, %llu drawn; %s",
                (unsigned long long) g.frame,
                (unsigned long long) g.changeListsSubmitted,
                (unsigned long long) g.changeListsDrawn,
                g.gpuIdle ? "nothing left to upload" : "uploading");
    ImGui::Text("tables: %u draw slots, %u meshes, %u materials, %u textures", g.drawSlots, g.meshes, g.materials, g.textures);

    ImGui::SeparatorText("Upload queue");
    note("Copies queued by the change lists, recorded by the SceneUpload pass before culling, at most the budget per frame.");
    const auto& u = g.uploads;
    ImGui::Text("queued: %u uploads, %.1f MiB; budget %.0f MiB per frame", u.queued, mib(u.queuedBytes), mib(u.budget));
    ImGui::Text("last pass: %u uploads, %.1f MiB at frame %llu; %.1f MiB in total", u.lastUploads, mib(u.lastBytes), (unsigned long long) u.lastFrame, mib(u.totalBytes));
    ImGui::Text("textures copied, waiting for their slot: %u", u.texturesWaitingSlot);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputInt("Budget (MiB)", &state.uploadBudgetMiB);
    ImGui::SameLine();
    if (ImGui::Button("Apply##budget")) {
        state.uploadBudgetRequested = (uint64_t) std::max(1, state.uploadBudgetMiB) * 1024 * 1024;
    }

    ImGui::SeparatorText("Geometry pool");
    note("Every mesh's vertices and indices, a range each; grows by doubling up to the limit. A mesh that doesn't fit waits, undrawn.");
    const auto& p = g.geometryPool;
    poolBar("vertices", p.vertexUsed, p.vertexCapacity, p.vertexLimit);
    poolBar("indices", p.indexUsed, p.indexCapacity, p.indexLimit);
    ImGui::Text("limit %.0f MiB per buffer", mib(p.limitBytes));
    if (p.waitingMeshes > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "waiting for room: %u meshes, %.1f MiB", p.waitingMeshes, mib(p.waitingBytes));
    } else {
        ImGui::Text("nothing waits for room");
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputInt("Limit (MiB, 0 device)", &state.geometryLimitMiB);
    ImGui::SameLine();
    if (ImGui::Button("Apply##limit")) {
        state.geometryLimitRequested = (uint64_t) std::max(0, state.geometryLimitMiB) * 1024 * 1024;
    }
}

auto drawResidency(const SceneFlowWindowState& state) -> void {
    const auto& r = state.residency;
    if (!stage("5. Residency", "Resident: the scene thread is idle and the render thread has drawn every change list with nothing left to upload. Script frames count only then.")) {
        return;
    }
    if (r.resident) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "resident");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "loading: %llu frames, longest %.1f ms", (unsigned long long) r.loadingFrames, r.longestLoadingFrameMs);
    }
    ImGui::Text("scene thread %s, render thread %s", state.thread.idle ? "idle" : "working", r.renderThreadDrawn ? "drawn" : "behind");
    ImGui::Text("script: frame %llu%s", (unsigned long long) r.scriptFrame, r.scriptHeld ? ", held" : "");
    if (r.everResident) {
        ImGui::Text("last resident at frame %llu, %.0f ms after start; that load took %llu frames, the longest %.1f ms",
                    (unsigned long long) r.lastResidentFrame,
                    r.lastResidentMs,
                    (unsigned long long) r.lastLoadFrames,
                    r.lastLoadLongestMs);
    }
}

} // namespace

auto drawSceneFlowWindow(bool& show, SceneFlowWindowState& state) -> void {
    if (!show) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(640, 720), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Scene Flow", &show)) {
        ImGui::End();
        return;
    }
    drawEngineMapLink("scene.compose");
    note("From the asset server to the screen, in the order data flows. Records: scenethread, gpuscene, assets.");
    drawAssets(state);
    arrow("packs arrive; the scene thread wakes");
    drawThread(state);
    arrow("3. change lists: what extract changed, taken by the main thread each frame and handed to the render thread");
    drawGpu(state);
    arrow("drawn by the frame graph's passes (Frame Graph window)");
    drawResidency(state);
    ImGui::End();
}
