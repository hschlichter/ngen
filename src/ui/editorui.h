#pragma once

#include "assetbrowser.h"     // AssetBrowserState
#include "framegraphdebug.h"
#include "propertieswindow.h" // PropertiesWindowState
#include "scenehandles.h"
#include "scenewindow.h" // SceneWindowState

#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <utility>

class DebugDraw;
class MaterialLibrary;
class MeshLibrary;
class SceneQuerySystem;
class SceneUpdater;
class USDRenderExtractor;
class USDScene;
struct Camera;
struct RenderWorld;
struct SDL_Window;

enum class EditorTool : uint8_t {
    Select,
    Translate,
    Rotate,
    Scale,
};

class EditorUI {
public:
    auto draw(SDL_Window* window,
              USDScene& usdScene,
              SceneUpdater& sceneUpdater,
              RenderWorld& renderWorld,
              PrimHandle& selectedPrim,
              const SceneQuerySystem& sceneQuery,
              const MaterialLibrary& matLib,
              Camera& camera,
              std::optional<FrameGraphDebugSnapshot> freshFrameGraphSnap) -> void;

    auto openScene(const char* path,
                   USDScene& usdScene,
                   USDRenderExtractor& usdExtractor,
                   MeshLibrary& meshLib,
                   MaterialLibrary& matLib,
                   RenderWorld& renderWorld,
                   SceneQuerySystem& sceneQuery,
                   SceneUpdater& sceneUpdater,
                   PrimHandle& selectedPrim) -> bool;

    auto drawDebug(DebugDraw& debugDraw,
                   const RenderWorld& renderWorld,
                   PrimHandle selectedPrim,
                   const SceneQuerySystem& sceneQuery,
                   const SceneUpdater& sceneUpdater,
                   USDScene& usdScene,
                   glm::vec3 cameraPos,
                   glm::vec3 worldUp) -> void;

    auto hasPendingOpen() const -> bool { return !pendingOpenPath.empty(); }
    auto consumePendingOpenPath() -> std::string { return std::exchange(pendingOpenPath, {}); }
    auto wantsQuit() const -> bool { return requestQuit; }

    // Hide all editor panels (Scene / Properties / Layers) if any are visible,
    // otherwise show all three. Bound to Ctrl+E and the Windows menu.
    auto togglePanels() -> void;
    auto getShowGizmo() const -> bool { return showGizmoFlag; }
    auto getGBufferViewMode() const -> int { return gbufferViewMode; }
    auto getShowBufferOverlay() const -> bool { return showBufferOverlayFlag; }
    auto getShowShadowOverlay() const -> bool { return showShadowOverlayFlag; }
    auto getShowFrameGraphWindow() const -> bool { return showFrameGraphWindow; }
    auto getShowAssetBrowserWindow() const -> bool { return showAssetBrowserWindow; }
    auto activeTool() const -> EditorTool { return activeToolValue; }
    auto setActiveTool(EditorTool t) -> void { activeToolValue = t; }
    auto setAssetBrowserRoot(std::string rootDir) -> void {
        assetBrowser.rootDir = std::move(rootDir);
        assetBrowser.selected.clear();
        invalidateAssetBrowser(assetBrowser);
    }

private:
    bool showSceneWindow = false;
    bool showPropertiesWindow = false;
    bool showLayersWindow = false;
    bool showToolsWindow = false;
    bool showUndoWindow = false;
    bool showGridFlag = true;
    bool showOriginFlag = true;
    bool showGizmoFlag = true;
    bool showAABBsFlag = false;
    bool showSelectedAABBFlag = true;
    bool showLightGizmosFlag = true;
    int gbufferViewMode = 0;
    bool showBufferOverlayFlag = false;
    bool showShadowOverlayFlag = false;
    bool showFrameGraphWindow = false;
    bool showAssetBrowserWindow = false;
    bool requestQuit = false;
    EditorTool activeToolValue = EditorTool::Select;
    PropertiesWindowState propertiesState;
    SceneWindowState sceneState;
    AssetBrowserState assetBrowser;
    std::string pendingOpenPath;
    std::optional<FrameGraphDebugSnapshot> fgLastSnapshot;
    std::optional<uint32_t> fgSelectedPass;
    std::optional<uint32_t> fgSelectedResource;
};
