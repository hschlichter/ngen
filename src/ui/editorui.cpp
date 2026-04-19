#include "editorui.h"

#include "assetbrowserwindow.h"
#include "debugdraw.h"
#include "framegraphwindow.h"
#include "layerswindow.h"
#include "mainmenubar.h"
#include "propertieswindow.h"
#include "renderworld.h"
#include "scenequery.h"
#include "sceneupdater.h"
#include "scenewindow.h"
#include "toolswindow.h"
#include "undowindow.h"
#include "usdrenderextractor.h"
#include "usdscene.h"

#include <functional>
#include <print>
#include <utility>

auto EditorUI::togglePanels() -> void {
    bool target = !(showSceneWindow || showPropertiesWindow || showLayersWindow || showToolsWindow);
    showSceneWindow = target;
    showPropertiesWindow = target;
    showLayersWindow = target;
    showToolsWindow = target;
}

auto EditorUI::draw(SDL_Window* window,
                    USDScene& usdScene,
                    SceneUpdater& sceneUpdater,
                    RenderWorld& renderWorld,
                    PrimHandle& selectedPrim,
                    const SceneQuerySystem& sceneQuery,
                    const MaterialLibrary& matLib,
                    Camera& camera,
                    std::optional<FrameGraphDebugSnapshot> freshFrameGraphSnap) -> void {
    if (freshFrameGraphSnap.has_value()) {
        fgLastSnapshot = std::move(freshFrameGraphSnap);
    }
    MainMenuBarState menuState{
        .showSceneWindow = showSceneWindow,
        .showPropertiesWindow = showPropertiesWindow,
        .showLayersWindow = showLayersWindow,
        .showToolsWindow = showToolsWindow,
        .showUndoWindow = showUndoWindow,
        .showGrid = showGridFlag,
        .showOrigin = showOriginFlag,
        .showGizmo = showGizmoFlag,
        .showAABBs = showAABBsFlag,
        .showSelectedAABB = showSelectedAABBFlag,
        .showLightGizmos = showLightGizmosFlag,
        .gbufferView = gbufferViewMode,
        .showBufferOverlay = showBufferOverlayFlag,
        .showShadowOverlay = showShadowOverlayFlag,
        .showFrameGraph = showFrameGraphWindow,
        .showAssetBrowser = showAssetBrowserWindow,
        .requestQuit = requestQuit,
        .pendingOpenPath = pendingOpenPath,
        .window = window,
        .sceneOpen = usdScene.isOpen(),
        .sceneUpdater = &sceneUpdater,
        .usdScene = &usdScene,
        .sceneQuery = &sceneQuery,
        .camera = &camera,
        .selectedPrim = &selectedPrim,
    };
    drawMainMenuBar(menuState);
    drawLayersWindow(showLayersWindow, sceneUpdater.isBlocked(), usdScene, sceneUpdater.edits());
    drawSceneWindow(showSceneWindow, sceneUpdater.isBlocked(), usdScene, renderWorld, selectedPrim, sceneState);
    drawPropertiesWindow(showPropertiesWindow, sceneUpdater.isBlocked(), usdScene, selectedPrim, sceneQuery, matLib, sceneUpdater.edits(), propertiesState);
    drawToolsWindow(showToolsWindow, activeToolValue);
    drawUndoWindow(showUndoWindow, sceneUpdater, usdScene);
    drawFrameGraphWindow(showFrameGraphWindow, fgLastSnapshot, fgSelectedPass, fgSelectedResource);
    drawAssetBrowserWindow(showAssetBrowserWindow, usdScene, assetBrowser, sceneUpdater.edits());
}

auto EditorUI::openScene(const char* path,
                         USDScene& usdScene,
                         USDRenderExtractor& usdExtractor,
                         MeshLibrary& meshLib,
                         MaterialLibrary& matLib,
                         RenderWorld& renderWorld,
                         SceneQuerySystem& sceneQuery,
                         SceneUpdater& sceneUpdater,
                         PrimHandle& selectedPrim) -> bool {
    sceneUpdater.waitIfBlocked();
    sceneUpdater.edits().clear();
    sceneUpdater.undoStack().clear();
    if (usdScene.isOpen()) {
        usdScene.close();
    }
    meshLib = {};
    matLib = {};
    renderWorld.clear();
    selectedPrim = {};

    if (!usdScene.open(path)) {
        std::println(stderr, "Failed to open: {}", path);
        return false;
    }
    usdScene.updateAssetBindings(meshLib, matLib);
    usdExtractor.extract(usdScene, meshLib, renderWorld);
    sceneQuery.rebuild(usdScene, meshLib);
    // Browser root is the CWD, set once at startup — don't touch it on scene change.
    // Just clear any stale selection from the previous scene.
    assetBrowser.selected.clear();
    showSceneWindow = true;
    showPropertiesWindow = true;
    showLayersWindow = true;
    showToolsWindow = true;
    showUndoWindow = true;
    return true;
}

auto EditorUI::drawDebug(DebugDraw& debugDraw,
                         const RenderWorld& renderWorld,
                         PrimHandle selectedPrim,
                         const SceneQuerySystem& sceneQuery,
                         const SceneUpdater& sceneUpdater,
                         USDScene& usdScene,
                         glm::vec3 cameraPos,
                         glm::vec3 worldUp) -> void {
    debugDraw.newFrame();
    if (showGridFlag) {
        debugDraw.grid(cameraPos, worldUp, 1.0f, 50, {0.25f, 0.25f, 0.25f, 1.0f});
    }
    if (showOriginFlag) {
        debugDraw.sphere({0, 0, 0}, 0.1f, {1.0f, 0.9f, 0.2f, 1.0f}, 16);
    }
    if (showAABBsFlag) {
        for (const auto& inst : renderWorld.meshInstances) {
            if (inst.worldBounds.valid()) {
                debugDraw.box(inst.worldBounds, {0.0f, 1.0f, 0.0f, 1.0f});
            }
        }
    }
    if (showLightGizmosFlag && !renderWorld.lights.empty()) {
        // Size the gizmo to the scene: compute a bounding sphere over the mesh instances and
        // place each directional light's sun disc "up in the sky" along the toward-light
        // direction. A distant light's authored origin is physically meaningless, so anchoring
        // to the scene makes the visualization match what the shader actually does.
        glm::vec3 mn(std::numeric_limits<float>::max());
        glm::vec3 mx(std::numeric_limits<float>::lowest());
        bool haveBounds = false;
        for (const auto& inst : renderWorld.meshInstances) {
            if (inst.worldBounds.valid()) {
                mn = glm::min(mn, inst.worldBounds.min);
                mx = glm::max(mx, inst.worldBounds.max);
                haveBounds = true;
            }
        }
        auto sceneCenter = haveBounds ? (mn + mx) * 0.5f : glm::vec3(0.0f);
        auto sceneRadius = haveBounds ? std::max(0.5f, glm::length(mx - mn) * 0.5f) : 1.0f;

        for (const auto& l : renderWorld.lights) {
            if (l.type != LightType::Directional) {
                continue;
            }
            // worldTransform[2] is the light's +Z axis — "toward the light" by USD convention.
            auto toward = glm::vec3(l.worldTransform[2]);
            if (glm::dot(toward, toward) < 1e-6f) {
                continue;
            }
            auto towardUnit = glm::normalize(toward);
            auto sunPos = sceneCenter + towardUnit * (sceneRadius * 1.5f);
            auto outgoing = -towardUnit;
            debugDraw.sunLight(sunPos, outgoing, sceneRadius * 0.1f, sceneRadius * 0.5f, {1.0f, 0.85f, 0.2f, 1.0f});
        }
    }
    if (selectedPrim && showSelectedAABBFlag && !sceneUpdater.isBlocked()) {
        std::function<void(PrimHandle)> highlightSubtree = [&](PrimHandle h) {
            const auto* bc = sceneQuery.bounds().get(h);
            if (bc && bc->worldBounds.valid()) {
                debugDraw.box(bc->worldBounds, {1.0f, 0.0f, 0.6f, 1.0f});
            }
            auto child = usdScene.firstChild(h);
            while (child) {
                highlightSubtree(child);
                child = usdScene.nextSibling(child);
            }
        };
        highlightSubtree(selectedPrim);
    }
}
