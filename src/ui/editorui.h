#pragma once

#include "propertieswindow.h" // PropertiesWindowState
#include "scenehandles.h"

#include <glm/glm.hpp>
#include <string>
#include <utility>

class DebugDraw;
class MaterialLibrary;
class MeshLibrary;
class SceneQuerySystem;
class SceneUpdater;
class USDRenderExtractor;
class USDScene;
struct RenderWorld;
struct SDL_Window;

enum class EditorTool : uint8_t {
    Translate,
    Rotate, // not yet implemented — dummy entry
    Scale,  // not yet implemented — dummy entry
};

class EditorUI {
public:
    auto draw(SDL_Window* window,
              USDScene& usdScene,
              SceneUpdater& sceneUpdater,
              RenderWorld& renderWorld,
              PrimHandle& selectedPrim,
              const SceneQuerySystem& sceneQuery,
              const MaterialLibrary& matLib) -> void;

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
                   glm::vec3 cameraPos) -> void;

    auto hasPendingOpen() const -> bool { return !pendingOpenPath.empty(); }
    auto consumePendingOpenPath() -> std::string { return std::exchange(pendingOpenPath, {}); }
    auto wantsQuit() const -> bool { return requestQuit; }

    // Hide all editor panels (Scene / Properties / Layers) if any are visible,
    // otherwise show all three. Bound to Ctrl+E and the Windows menu.
    auto togglePanels() -> void;
    auto getShowGizmo() const -> bool { return showGizmoFlag; }
    auto getGBufferViewMode() const -> int { return gbufferViewMode; }
    auto getShowBufferOverlay() const -> bool { return showBufferOverlayFlag; }
    auto activeTool() const -> EditorTool { return activeToolValue; }

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
    int gbufferViewMode = 0;
    bool showBufferOverlayFlag = false;
    bool requestQuit = false;
    EditorTool activeToolValue = EditorTool::Translate;
    PropertiesWindowState propertiesState;
    std::string pendingOpenPath;
};
