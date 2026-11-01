#include "statusbar.h"

#include "assetclient.h"
#include "drawlists.h"
#include "rhidevice.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

// The process's resident set, from /proc/self/statm (pages).
auto residentBytes() -> uint64_t {
    std::ifstream statm("/proc/self/statm");
    uint64_t size = 0;
    uint64_t resident = 0;
    statm >> size >> resident;
    return resident * (uint64_t) sysconf(_SC_PAGESIZE);
}

auto bytesText(uint64_t bytes) -> std::string {
    char text[32];
    if (bytes >= 1024ull * 1024 * 1024) {
        std::snprintf(text, sizeof(text), "%.2f GB", (double) bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024) {
        std::snprintf(text, sizeof(text), "%.1f MB", (double) bytes / (1024.0 * 1024.0));
    } else {
        std::snprintf(text, sizeof(text), "%.1f KB", (double) bytes / 1024.0);
    }
    return text;
}

auto countText(uint64_t count) -> std::string {
    char text[32];
    if (count >= 1'000'000) {
        std::snprintf(text, sizeof(text), "%.2fM", (double) count / 1'000'000.0);
    } else if (count >= 1'000) {
        std::snprintf(text, sizeof(text), "%.1fK", (double) count / 1'000.0);
    } else {
        std::snprintf(text, sizeof(text), "%llu", (unsigned long long) count);
    }
    return text;
}

} // namespace

auto drawStatusBar(const StatusBarData& data) -> void {
    auto flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    auto height = ImGui::GetFrameHeight();
    if (!ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, height, flags)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginMenuBar()) {
        char left[128];
        if (data.gpuMs > 0.0) {
            std::snprintf(left, sizeof(left), "%.1f fps   %.2f ms   GPU %.2f ms", data.fps, data.frameMs, data.gpuMs);
        } else {
            std::snprintf(left, sizeof(left), "%.1f fps   %.2f ms   GPU -", data.fps, data.frameMs);
        }
        std::string leftText = left;
        leftText += "   |   " + countText(data.trianglesDrawn) + " / " + countText(data.trianglesScene) + " tris";
        char speed[48];
        std::snprintf(speed, sizeof(speed), "   |   camera %.1f m/s", data.cameraSpeed);
        leftText += speed;
        ImGui::TextUnformatted(leftText.c_str());

        std::string right = "CPU " + bytesText(data.cpuMemoryBytes) + "   GPU " + bytesText(data.gpuMemoryBytes) + "   |   ";
        if (data.assetsConnected) {
            char assets[256];
            std::snprintf(assets,
                          sizeof(assets),
                          "assets %llu requested  %llu packed  %llu cached  %llu failed  %llu in flight  %s",
                          (unsigned long long) data.assetsRequested,
                          (unsigned long long) data.assetsPacked,
                          (unsigned long long) data.assetsCached,
                          (unsigned long long) data.assetsFailed,
                          (unsigned long long) data.assetsInFlight,
                          bytesText(data.assetBytes).c_str());
            right += assets;
        } else {
            right += "no asset server";
        }
        auto width = ImGui::CalcTextSize(right.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - width - ImGui::GetStyle().WindowPadding.x * 2.0f);
        if (data.assetsFailed > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", right.c_str());
        } else {
            ImGui::TextUnformatted(right.c_str());
        }
        ImGui::EndMenuBar();
    }
    ImGui::End();
}

auto StatusBarGatherer::gather(const RhiDevice& device, const AssetClient& assets, const CullResult& cull) -> StatusBarData {
    data.trianglesDrawn = cull.cameraTriangles;
    data.trianglesScene = cull.sceneTriangles;
    profile::frameHistory(frames);
    // Average over the last second of frames, newest first.
    double cpuSum = 0.0;
    double gpuSum = 0.0;
    size_t count = 0;
    size_t gpuCount = 0;
    for (auto it = frames.rbegin(); it != frames.rend() && cpuSum < 1000.0; ++it) {
        cpuSum += it->cpuMs;
        count++;
        if (it->gpuMs > 0.0) {
            gpuSum += it->gpuMs;
            gpuCount++;
        }
    }
    if (count > 0 && cpuSum > 0.0) {
        data.frameMs = cpuSum / (double) count;
        data.fps = 1000.0 / data.frameMs;
    }
    data.gpuMs = gpuCount > 0 ? gpuSum / (double) gpuCount : 0.0;

    auto now = profile::now();
    if (now - lastMemorySampleNs >= 250'000'000ull || lastMemorySampleNs == 0) {
        lastMemorySampleNs = now;
        data.cpuMemoryBytes = residentBytes();
        device.memoryHeaps(heaps);
        data.gpuMemoryBytes = 0;
        for (const auto& heap : heaps) {
            data.gpuMemoryBytes += heap.allocated;
        }
    }

    auto stats = assets.stats();
    data.assetsConnected = true;
    data.assetsRequested = stats.requested;
    data.assetsPacked = stats.packed;
    data.assetsCached = stats.cached;
    data.assetsFailed = stats.failed;
    data.assetsInFlight = stats.inFlight();
    data.assetBytes = stats.bytes;
    return data;
}
