#pragma once

#include "editcommand.h"
#include "jobsystem.h"
#include "material.h"
#include "mesh.h"
#include "renderworld.h"
#include "scenequery.h"

class USDScene;
class USDRenderExtractor;

class SceneUpdater {
public:
    auto update(USDScene& usdScene,
                USDRenderExtractor& usdExtractor,
                RenderWorld& renderWorld,
                MeshLibrary& meshLib,
                MaterialLibrary& matLib,
                SceneQuerySystem& sceneQuery) -> bool;

    auto waitIfBlocked() -> void;

    auto addEdit(SceneEditCommand cmd) -> void { pendingEdits.push_back(std::move(cmd)); }
    auto edits() -> std::vector<SceneEditCommand>& { return pendingEdits; }
    auto isBlocked() const -> bool { return editingBlocked; }

private:
    JobFence sceneUpdateFence;
    bool editingBlocked = false;
    std::vector<SceneEditCommand> pendingEdits;
    RenderWorld pendingRenderWorld;
    MeshLibrary pendingMeshLib;
    MaterialLibrary pendingMatLib;
    SceneQuerySystem pendingSceneQuery;
};
