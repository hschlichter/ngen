#pragma once

#include "editcommand.h"
#include "jobsystem.h"
#include "material.h"
#include "mesh.h"
#include "renderworld.h"
#include "scenequery.h"
#include "undostack.h"

class USDScene;
class USDRenderExtractor;

// Result of a single SceneUpdater::update() call. Lets callers cheaply decide
// whether to refresh shared_ptrs to the mesh/material libraries (which only
// change on the async batch swap path).
enum class SceneUpdateResult : uint8_t {
    None,           // nothing changed this frame
    TransformsOnly, // fast path applied — RenderWorld changed, libraries did not
    Full,           // async batch swap — libraries may have changed
};

class SceneUpdater {
public:
    auto update(USDScene& usdScene,
                USDRenderExtractor& usdExtractor,
                RenderWorld& renderWorld,
                MeshLibrary& meshLib,
                MaterialLibrary& matLib,
                SceneQuerySystem& sceneQuery) -> SceneUpdateResult;

    auto waitIfBlocked() -> void;

    auto addEdit(SceneEditCommand cmd) -> void { pendingEdits.push_back(std::move(cmd)); }
    auto edits() -> std::vector<SceneEditCommand>& { return pendingEdits; }
    auto isBlocked() const -> bool { return editingBlocked; }

    auto undoStack() -> UndoStack& { return m_undoStack; }
    auto undoStack() const -> const UndoStack& { return m_undoStack; }

private:
    JobFence sceneUpdateFence;
    bool editingBlocked = false;
    std::vector<SceneEditCommand> pendingEdits;
    UndoStack m_undoStack;
    RenderWorld pendingRenderWorld;
    MeshLibrary pendingMeshLib;
    MaterialLibrary pendingMatLib;
    SceneQuerySystem pendingSceneQuery;
};
