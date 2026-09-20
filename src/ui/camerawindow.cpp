#include "camerawindow.h"

#include "camera.h"
#include "sessionscript.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

auto loadBookmarks(CameraWindowState& state) -> void {
    state.loaded = true;
    state.bookmarks.clear();
    if (state.bookmarkPath.empty()) {
        return;
    }
    std::ifstream file(state.bookmarkPath);
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream in(line);
        CameraBookmark b;
        if (in >> b.name >> b.pose[0] >> b.pose[1] >> b.pose[2] >> b.pose[3] >> b.pose[4]) {
            state.bookmarks.push_back(b);
        }
    }
}

auto saveBookmarks(const CameraWindowState& state) -> void {
    if (state.bookmarkPath.empty()) {
        return;
    }
    std::ofstream file(state.bookmarkPath);
    for (const auto& b : state.bookmarks) {
        file << b.name << ' ' << b.pose[0] << ' ' << b.pose[1] << ' ' << b.pose[2] << ' ' << b.pose[3] << ' ' << b.pose[4] << '\n';
    }
}

auto applyPose(Camera& camera, const float pose[5]) -> void {
    camera.position = glm::vec3(pose[0], pose[1], pose[2]);
    camera.yaw = pose[3];
    camera.pitch = pose[4];
}

} // namespace

void drawCameraWindow(bool& show, Camera& camera, CameraWindowState& state) {
    if (!show) {
        return;
    }
    if (!state.loaded) {
        loadBookmarks(state);
    }
    ImGui::SetNextWindowSize(ImVec2(460, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Camera", &show)) {
        ImGui::End();
        return;
    }

    float pose[5] = {camera.position.x, camera.position.y, camera.position.z, camera.yaw, camera.pitch};
    auto poseText = formatCameraPose(pose);
    ImGui::TextUnformatted("Current pose (x,y,z,yaw,pitch):");
    ImGui::TextUnformatted(poseText.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        ImGui::SetClipboardText(poseText.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy as flag")) {
        ImGui::SetClipboardText(("--camera=" + poseText).c_str());
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(-80.0f);
    bool jump = ImGui::InputText("##jump", state.jumpText, sizeof(state.jumpText), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    jump = ImGui::Button("Jump") || jump;
    if (jump) {
        float target[5] = {};
        if (parseCameraPose(state.jumpText, target)) {
            applyPose(camera, target);
        }
    }
    if (ImGui::Button("Frame scene")) {
        state.requestFrameScene = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Frame selected")) {
        state.requestFrameSelected = true;
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Bookmarks");
    if (state.bookmarkPath.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(no scene file: not saved)");
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("##name", state.newName, sizeof(state.newName));
    ImGui::SameLine();
    if (ImGui::Button("Add current") && state.newName[0] != '\0') {
        CameraBookmark b;
        b.name = state.newName;
        for (auto& c : b.name) {
            if (c == ' ') {
                c = '_';
            }
        }
        std::memcpy(b.pose, pose, sizeof(pose));
        state.bookmarks.push_back(b);
        state.newName[0] = '\0';
        saveBookmarks(state);
    }
    int removeIndex = -1;
    for (size_t i = 0; i < state.bookmarks.size(); i++) {
        auto& b = state.bookmarks[i];
        ImGui::PushID((int) i);
        if (ImGui::Button("Go")) {
            applyPose(camera, b.pose);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            removeIndex = (int) i;
        }
        ImGui::SameLine();
        ImGui::Text("%s  %s", b.name.c_str(), formatCameraPose(b.pose).c_str());
        ImGui::PopID();
    }
    if (removeIndex >= 0) {
        state.bookmarks.erase(state.bookmarks.begin() + removeIndex);
        saveBookmarks(state);
    }
    ImGui::End();
}
