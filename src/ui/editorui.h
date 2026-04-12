#pragma once

#include "scenehandles.h"

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
                   USDScene& usdScene) -> void;

    auto hasPendingOpen() const -> bool { return !pendingOpenPath.empty(); }
    auto consumePendingOpenPath() -> std::string { return std::exchange(pendingOpenPath, {}); }
    auto wantsQuit() const -> bool { return requestQuit; }
    auto getGBufferViewMode() const -> int { return gbufferViewMode; }
    auto getShowBufferOverlay() const -> bool { return showBufferOverlayFlag; }

private:
    bool showSceneWindow = false;
    bool showPropertiesWindow = false;
    bool showLayersWindow = false;
    bool showAABBsFlag = false;
    bool showSelectedAABBFlag = true;
    int gbufferViewMode = 0;
    bool showBufferOverlayFlag = false;
    bool requestQuit = false;
    std::string pendingOpenPath;
};
