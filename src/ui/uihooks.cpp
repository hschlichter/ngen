#include "uihooks.h"

#include "assetclient.h"
#include "imguibackendvulkan.h"
#include "profile.h"
#include "renderdoccapture.h"
#include "renderer.h"
#include "renderthread.h"
#include "toolshooks.h"
#include "trace.h"
#include "viewframe.h"
#include "viewscene.h"
#include "viewui.h"

#include <SDL3/SDL_events.h>
#include <imgui.h>

#include <format>
#include <string>
#include <utility>

auto UiHooks::afterScene(ViewFrame& frame) -> void {
    auto& state = tools.state();
    state.lod.windowOpen = ui.wantsLodWindow();
    state.streaming.windowOpen = ui.wantsStreamingWindow();
    // The cameras the scene authors, as poses for the Camera window: built only while it is open, once per scene snapshot.
    if (ui.wantsCameraWindow() && cameraListSnapshot != frame.snapshotsTaken) {
        cameraListSnapshot = frame.snapshotsTaken;
        PROFILE_ZONE("SceneWindowLists");
        auto& sceneCameras = ui.cameraWindow().sceneCameras;
        sceneCameras.clear();
        for (const auto& authored : frame.scene.cameras()) {
            Camera placed = frame.cam;
            placed.place(authored.worldTransform);
            sceneCameras.push_back({.name = authored.path, .pose = {placed.position.x, placed.position.y, placed.position.z, placed.yaw, placed.pitch}});
        }
    }
    tools.afterScene(frame);
}

auto UiHooks::cameras(ViewFrame& frame) -> void {
    tools.cameras(frame);
}

auto UiHooks::event(ViewFrame& frame, const SDL_Event& event) -> bool {
    {
        PROFILE_ZONE("ImGuiEvent");
        if (imgui.processEvent(&event)) {
            return true;
        }
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F12 && !event.key.repeat) {
        ui.requestScreenshot();
        return true;
    }
    return tools.event(frame, event);
}

auto UiHooks::afterCamera(ViewFrame& frame) -> void {
    tools.afterCamera(frame);
    const auto& cull = frame.latestCull;
    ui.setCullStats(cull.instances, cull.cameraCulled, cull.lodDropped);
    ui.setShadowCullStats(cull.cascadeCount, cull.cascadeCulled, cull.cascadeDrawn);
    ui.setCullTriangles(cull.cameraTriangles, cull.sceneTriangles, cull.cascadeTriangles);
}

auto UiHooks::debugDraw(ViewFrame& frame) -> void {
    tools.debugDraw(frame);
}

auto UiHooks::wantReadbacks(ViewFrame& frame) -> void {
    auto& readbacks = frame.readbacks;
    readbacks.frameGraphDebug = readbacks.frameGraphDebug || ui.wantsFrameGraphDebug();
    readbacks.renderDebug = readbacks.renderDebug || ui.wantsRenderDebug();
    readbacks.counters = readbacks.counters || ui.wantsCounters();
    readbacks.frameDebug = ui.takeFrameDebugRequest() || readbacks.frameDebug;
    auto watches = ui.captureWatches();
    readbacks.watches.insert(readbacks.watches.end(), watches.begin(), watches.end());
}

auto UiHooks::takeReadbacks(ViewFrame& frame) -> void {
    auto& readbacks = frame.readbacks;
    if (readbacks.frameDebugCapture.has_value()) {
        ui.onFrameDebugCapture(std::move(*readbacks.frameDebugCapture));
    }
    for (auto& result : readbacks.captures) {
        ui.onCaptureResult(std::move(result));
    }
    readbacks.captures.clear();
    for (auto& counters : readbacks.counterFrames) {
        ui.onCounters(std::move(counters));
    }
}

auto UiHooks::drawUi(ViewFrame& frame) -> void {
    static const uint32_t uiZoneId = profile::registerName("EditorUI");
    profile::beginZone(uiZoneId);
    auto& scene = frame.scene;
    {
        PROFILE_ZONE("ImGuiBeginFrame");
        imgui.beginFrame();
    }
    {
        PROFILE_ZONE("StatusBarGather");
        auto statusBarData = statusBar.gather(device, frame.assets, frame.latestCull);
        statusBarData.cameraSpeed = (float) (frame.cameras.controlled->speed * scene.lodMetersPerUnit());
        ui.setStatusBar(statusBarData);
    }
    if (ui.wantsAssetLog()) {
        ui.setAssetLog(frame.assets.log());
    }
    // The Assets window: the selected prim's assets, a go-to, repacks (blocking, as loading the scene is; the rebuilt scene uploads as any
    // change does).
    ui.setHasPrimSelected(!frame.selectedPath.empty());
    if (ui.takeFindSelectedRequest()) {
        ui.selectAssets(scene.assetsUnder(frame.selectedPath));
    }
    if (auto goTo = ui.takeGoToRequest()) {
        auto result = goToAsset(frame.goTo, frame.cam, scene, frame.selectedPath, goTo->id, goTo->all);
        if (!result.error.empty()) {
            ui.setGoToStatus(result.error);
        } else if (goTo->all) {
            ui.setGoToStatus(std::format("{}: all {} draws framed", goTo->id, result.count));
        } else {
            ui.setGoToStatus(std::format("{}: draw {} of {}, {}", goTo->id, result.index + 1, result.count, result.prim));
        }
    }
    if (auto repack = ui.takeRepackRequest(); repack.scene) {
        scene.refresh(scene.assetIds(), true);
    } else if (!repack.ids.empty()) {
        scene.refresh(repack.ids, true);
    }
    {
        PROFILE_ZONE("ViewUIDraw");
        auto primPathOf = [&scene](uint32_t prim) -> std::string {
            return scene.primPath(prim);
        };
        ui.draw(primPathOf, !frame.selectedPath.empty(), scene.renderWorld(), frame.cam, std::move(frame.readbacks.frameGraph), std::move(frame.readbacks.renderDebugSnapshot));
    }
    {
        PROFILE_ZONE("LodLabels");
        const auto& io = ImGui::GetIO();
        auto viewProj = frame.proj * frame.cameras.view->viewMatrix();
        auto& state = tools.state();
        drawLodLabels(state.lod, viewProj, glm::vec2(io.DisplaySize.x, io.DisplaySize.y));
        drawStreamingLabels(state.streaming, viewProj, glm::vec2(io.DisplaySize.x, io.DisplaySize.y));
    }
    switch (ui.takeEditRequest()) {
        case ViewEditRequest::Deselect:
            frame.deselect();
            break;
        case ViewEditRequest::SelectParent:
            frame.selectParent();
            break;
        case ViewEditRequest::FrameSelected:
            frame.frameSelected();
            break;
        case ViewEditRequest::None:
            break;
    }
    if (auto inspect = ui.takeTextureInspectRequest(); inspect.has_value()) {
        frame.renderThread.setTextureInspect(*inspect);
    }
    if (auto dump = ui.takeTextureDumpRequest(); dump.has_value()) {
        frame.renderer.requestTextureDump(dump->texture, dump->level, "texture_" + std::to_string(dump->texture) + "_L" + std::to_string(dump->level) + ".png");
    }
    {
        auto& flags = ui.introspection();
        flags.renderDocAvailable = renderDoc.available();
        if (flags.renderDocCaptureRequested) {
            flags.renderDocCaptureRequested = false;
            renderDoc.triggerCapture();
        }
        if (flags.renderDocOpenRequested) {
            flags.renderDocOpenRequested = false;
            if (!renderDoc.openInUi(renderDoc.lastCapture())) {
                TRACE_WARNING("Engine", "RenderDocOpenFailed", "renderdoc").text("RenderDoc: no capture to open");
            }
        }
        for (const auto& path : renderDoc.pollNewCaptures()) {
            TRACE_EVENT("Engine", "RenderDocCapture", path).text(std::format("RenderDoc capture written: {}", path)).field("frame", (int64_t) frame.frameCounter);
        }
    }
    if (ui.takeScreenshotRequest()) {
        frame.renderer.requestScreenshot("screenshot_" + std::to_string(frame.frameCounter) + ".png");
    }
    {
        auto& camWindow = ui.cameraWindow();
        if (camWindow.requestFrameScene) {
            camWindow.requestFrameScene = false;
            frame.frameScene();
        }
        if (camWindow.requestFrameSelected) {
            camWindow.requestFrameSelected = false;
            frame.frameSelected();
        }
    }
    // The tools carry out what their windows asked for this frame.
    tools.state().activeTool = ui.activeTool();
    tools.drawUi(frame);
    {
        PROFILE_ZONE("ImGuiRender");
        imguiSnapshot = imgui.endFrame();
    }
    if (ui.wantsQuit()) {
        frame.quit = true;
    }
    profile::endZone();
}

auto UiHooks::fillSnapshot(ViewFrame& frame, RenderSnapshot& snapshot) -> void {
    tools.fillSnapshot(frame, snapshot);
    snapshot.imguiSnapshot = std::move(imguiSnapshot);
}
