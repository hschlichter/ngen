#include "camerawindow.h"

#include "camera.h"
#include "sessionscript.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <format>
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

// The debug camera's section: the main camera is the pose above, which everything placing the camera moves.
auto drawDebugCamera(DebugCameraState& debug) -> void {
    ImGui::Checkbox("Debug camera", &debug.on);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Render from a second camera. The main camera above (the player) still decides what is resident, the LOD levels\nand, unless culling is set to it, nothing else; it is drawn as a frustum with its trail.");
    }
    if (!debug.on) {
        return;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Place behind main")) {
        debug.placeRequested = true;
    }
    float pose[5] = {debug.camera.position.x, debug.camera.position.y, debug.camera.position.z, debug.camera.yaw, debug.camera.pitch};
    ImGui::Text("Debug pose: %s", formatCameraPose(pose).c_str());
    int control = debug.controlMain ? 1 : 0;
    ImGui::TextUnformatted("Controls move");
    ImGui::SameLine();
    ImGui::RadioButton("debug camera", &control, 0);
    ImGui::SameLine();
    ImGui::RadioButton("main camera", &control, 1);
    debug.controlMain = control == 1;
    ImGui::SameLine();
    ImGui::TextDisabled("(C)");
    // What the main camera drives, each switchable to the debug camera.
    auto source = [](const char* label, const char* id, bool& fromDebug, const char* tooltip) {
        int from = fromDebug ? 1 : 0;
        ImGui::TextUnformatted(label);
        ImGui::SameLine(150.0f);
        ImGui::RadioButton(std::format("main##{}", id).c_str(), &from, 0);
        ImGui::SameLine();
        ImGui::RadioButton(std::format("debug##{}", id).c_str(), &from, 1);
        fromDebug = from == 1;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip);
        }
    };
    source("LOD and streaming", "lod", debug.lodFromDebug, "The LOD heuristic, what is resident and render LOD's pick, measured from the debug camera instead of the player.");
    source("Culling", "cull", debug.cullFromDebug, "Draw what the debug camera sees instead of what the player sees.");
    source("Shadows", "shadows", debug.shadowsFromDebug, "Fit the shadow cascades to the debug camera's view instead of the player's.");
    ImGui::Checkbox("Trail", &debug.trail);
    ImGui::SameLine();
    ImGui::Text("%zu points", debug.trailPoints.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        debug.trailPoints.clear();
    }
}

// The camera modes' section: following the ground and orbiting, for the camera that has the controls.
auto drawCameraModes(CameraModesState& modes) -> void {
    ImGui::Checkbox("Follow ground", &modes.follow);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Keep the eye a height above the highest drawn surface under it (render geometry: ground cover and roofs count).\nWASD move across the ground, Q/E change the height. G toggles it.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(G)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat("height (m)", &modes.followHeightMetres, 0.05f, 0.0f, 1000.0f, "%.2f");
    if (modes.follow) {
        if (modes.ground) {
            ImGui::Text("Ground at %.2f, %.2f, %.2f", modes.ground->x, modes.ground->y, modes.ground->z);
            ImGui::TextWrapped("on %s", modes.groundPrim.c_str());
        } else {
            ImGui::TextDisabled("No drawn surface under the camera: the height is held.");
        }
        ImGui::Text("Tested %u chunks, %u draws, %llu triangles in %.2f ms", modes.groundChunks, modes.groundDraws, (unsigned long long) modes.groundTriangles, modes.groundMs);
    }
    if (modes.orbiting) {
        ImGui::Text("Orbiting %.2f, %.2f, %.2f%s", modes.pivot.x, modes.pivot.y, modes.pivot.z, modes.pivotPicked ? "" : " (the cursor's depth on its way)");
    } else {
        ImGui::TextDisabled("Hold Left Alt: right-drag orbits the surface under the cursor, the wheel moves toward it.");
    }
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

    ImGui::Text("Fly speed: %.1f m/s (mouse wheel to change)", camera.speed);

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
    drawCameraModes(state.modes);

    ImGui::Separator();
    drawDebugCamera(state.debug);

    ImGui::Separator();
    ImGui::Text("Scene cameras (%zu)", state.sceneCameras.size());
    if (state.sceneCameras.empty()) {
        ImGui::TextDisabled("(the scene authors none)");
    } else {
        // Eight rows high, scrolling past that.
        auto rows = (float) std::min<size_t>(state.sceneCameras.size(), 8);
        auto height = (rows * ImGui::GetTextLineHeightWithSpacing()) + (ImGui::GetStyle().FramePadding.y * 2.0f);
        if (ImGui::BeginListBox("##sceneCameras", ImVec2(-FLT_MIN, height))) {
            for (const auto& c : state.sceneCameras) {
                ImGui::PushID(c.name.c_str());
                if (ImGui::Selectable(c.name.c_str())) {
                    applyPose(camera, c.pose);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", formatCameraPose(c.pose).c_str());
                }
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
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
