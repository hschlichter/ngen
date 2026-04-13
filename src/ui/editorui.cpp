#include "editorui.h"

#include "debugdraw.h"
#include "layerswindow.h"
#include "mainmenubar.h"
#include "propertieswindow.h"
#include "renderworld.h"
#include "scenequery.h"
#include "sceneupdater.h"
#include "scenewindow.h"
#include "usdrenderextractor.h"
#include "usdscene.h"

#include <functional>
#include <print>

auto EditorUI::togglePanels() -> void {
    bool target = !(showSceneWindow || showPropertiesWindow || showLayersWindow);
    showSceneWindow = target;
    showPropertiesWindow = target;
    showLayersWindow = target;
}

auto EditorUI::draw(SDL_Window* window,
                    USDScene& usdScene,
                    SceneUpdater& sceneUpdater,
                    RenderWorld& renderWorld,
                    PrimHandle& selectedPrim,
                    const SceneQuerySystem& sceneQuery,
                    const MaterialLibrary& matLib) -> void {
    MainMenuBarState menuState{
        .showSceneWindow = showSceneWindow,
        .showPropertiesWindow = showPropertiesWindow,
        .showLayersWindow = showLayersWindow,
        .showGrid = showGridFlag,
        .showOrigin = showOriginFlag,
        .showGizmo = showGizmoFlag,
        .showAABBs = showAABBsFlag,
        .showSelectedAABB = showSelectedAABBFlag,
        .gbufferView = gbufferViewMode,
        .showBufferOverlay = showBufferOverlayFlag,
        .requestQuit = requestQuit,
        .pendingOpenPath = pendingOpenPath,
        .window = window,
        .sceneOpen = usdScene.isOpen(),
    };
    drawMainMenuBar(menuState);
    drawLayersWindow(showLayersWindow, sceneUpdater.isBlocked(), usdScene, sceneUpdater.edits());
    drawSceneWindow(showSceneWindow, sceneUpdater.isBlocked(), usdScene, renderWorld, selectedPrim);
    drawPropertiesWindow(showPropertiesWindow, sceneUpdater.isBlocked(), usdScene, selectedPrim, sceneQuery, matLib, sceneUpdater.edits());
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
    showSceneWindow = true;
    showPropertiesWindow = true;
    showLayersWindow = true;
    return true;
}

auto EditorUI::drawDebug(DebugDraw& debugDraw,
                         const RenderWorld& renderWorld,
                         PrimHandle selectedPrim,
                         const SceneQuerySystem& sceneQuery,
                         const SceneUpdater& sceneUpdater,
                         USDScene& usdScene,
                         glm::vec3 cameraPos) -> void {
    debugDraw.newFrame();
    if (showGridFlag) {
        debugDraw.grid(cameraPos, 1.0f, 50, {0.25f, 0.25f, 0.25f, 1.0f});
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
