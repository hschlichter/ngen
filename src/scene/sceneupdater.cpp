#include "sceneupdater.h"

#include "renderer.h"
#include "usdrenderextractor.h"
#include "usdscene.h"

auto SceneUpdater::update(USDScene& usdScene,
                          USDRenderExtractor& usdExtractor,
                          Renderer& renderer,
                          RenderWorld& renderWorld,
                          MeshLibrary& meshLib,
                          MaterialLibrary& matLib,
                          SceneQuerySystem& sceneQuery) -> void {
    // Phase 1: Swap in results from completed background job
    if (editingBlocked && sceneUpdateFence.ready()) {
        JobSystem::wait(sceneUpdateFence);
        renderWorld = std::move(pendingRenderWorld);
        meshLib = std::move(pendingMeshLib);
        matLib = std::move(pendingMatLib);
        sceneQuery = std::move(pendingSceneQuery);
        renderer.uploadRenderWorld(renderWorld, meshLib, matLib);
        editingBlocked = false;
    }

    // Phase 2: Kick off background job if edits are pending
    if (!editingBlocked && !pendingEdits.empty()) {
        editingBlocked = true;
        pendingMeshLib = meshLib;
        pendingMatLib = matLib;
        auto edits = std::move(pendingEdits);
        pendingEdits.clear();

        sceneUpdateFence = JobSystem::submit([&usdScene,
                                              &usdExtractor,
                                              &pendingRenderWorld = pendingRenderWorld,
                                              &pendingMeshLib = pendingMeshLib,
                                              &pendingMatLib = pendingMatLib,
                                              &pendingSceneQuery = pendingSceneQuery,
                                              edits = std::move(edits)] {
            for (const auto& cmd : edits) {
                switch (cmd.type) {
                    case SceneEditCommand::Type::MuteLayer:
                        usdScene.setLayerMuted(cmd.layer, cmd.boolValue);
                        break;
                    case SceneEditCommand::Type::SetTransform:
                        usdScene.setTransform(cmd.prim, cmd.transform);
                        break;
                    case SceneEditCommand::Type::SetVisibility:
                        usdScene.setVisibility(cmd.prim, cmd.boolValue);
                        break;
                    case SceneEditCommand::Type::AddSubLayer:
                        usdScene.addSubLayer(cmd.stringValue.c_str());
                        break;
                    case SceneEditCommand::Type::ClearSession:
                        usdScene.clearSessionLayer();
                        break;
                }
            }

            usdScene.beginFrame();
            usdScene.processChanges();
            usdScene.endFrame();

            const auto& dirty = usdScene.dirtySet();
            if (!dirty.primsResynced.empty()) {
                usdScene.updateAssetBindings(pendingMeshLib, pendingMatLib);
            }
            usdExtractor.extract(usdScene, pendingMeshLib, pendingRenderWorld);
            pendingSceneQuery.rebuild(usdScene, pendingMeshLib);
        });
    }

    // Phase 3: Normal frame processing (no edits, not blocked)
    if (!editingBlocked) {
        usdScene.beginFrame();
        usdScene.processChanges();
        usdScene.endFrame();

        const auto& dirty = usdScene.dirtySet();
        if (!dirty.primsResynced.empty() || !dirty.transformDirty.empty() || !dirty.assetsDirty.empty()) {
            if (!dirty.primsResynced.empty()) {
                usdScene.updateAssetBindings(meshLib, matLib);
            }
            usdExtractor.extract(usdScene, meshLib, renderWorld);
            renderer.uploadRenderWorld(renderWorld, meshLib, matLib);
            sceneQuery.rebuild(usdScene, meshLib);
        }
    }
}

auto SceneUpdater::waitIfBlocked() -> void {
    if (editingBlocked) {
        JobSystem::wait(sceneUpdateFence);
        editingBlocked = false;
    }
}
