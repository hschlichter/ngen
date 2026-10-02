#include "statusbar.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <string>

namespace {

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
