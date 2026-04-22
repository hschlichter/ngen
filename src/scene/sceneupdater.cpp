#include "sceneupdater.h"

#include "observationmacros.h"
#include "usdrenderextractor.h"
#include "usdscene.h"

#include <algorithm>
#include <unordered_map>

namespace {
auto resultToString(SceneUpdateResult r) -> const char* {
    switch (r) {
        case SceneUpdateResult::None:
            return "None";
        case SceneUpdateResult::TransformsOnly:
            return "TransformsOnly";
        case SceneUpdateResult::Full:
            return "Full";
    }
    return "Unknown";
}
} // namespace

// Walk the subtree under `root` and append all prim handles (including root) to `out`.
static auto appendSubtree(const USDScene& scene, PrimHandle root, std::vector<PrimHandle>& out) -> void {
    out.push_back(root);
    for (auto c = scene.firstChild(root); (bool) c; c = scene.nextSibling(c)) {
        appendSubtree(scene, c, out);
    }
}

auto SceneUpdater::update(
    USDScene& usdScene, USDRenderExtractor& usdExtractor, RenderWorld& renderWorld, MeshLibrary& meshLib, MaterialLibrary& matLib, SceneQuerySystem& sceneQuery)
    -> SceneUpdateResult {
    auto result = SceneUpdateResult::None;

    // Phase 1: Swap in results from completed background job
    if (editingBlocked && sceneUpdateFence.ready()) {
        JobSystem::wait(sceneUpdateFence);
        renderWorld = std::move(pendingRenderWorld);
        meshLib = std::move(pendingMeshLib);
        matLib = std::move(pendingMatLib);
        sceneQuery = std::move(pendingSceneQuery);
        editingBlocked = false;
        result = SceneUpdateResult::Full;
    }

    // Fast path: if every pending edit is a transform tweak (gizmo drag, Properties
    // scrubbing) and we're not currently waiting on a background job, apply them
    // synchronously and patch only the affected instances + BVH leaves. Avoids the
    // round-trip cost of the async pipeline for the common interactive case.
    //
    // Preview edits skip the USD layer write entirely (they only touch the runtime
    // transform cache); Authoring edits commit to USD as usual. Both paths drive
    // the same downstream patch — RenderWorld instance + BVH refit.
    if (!editingBlocked && !pendingEdits.empty() &&
        std::ranges::all_of(pendingEdits, [](const auto& e) { return e.type == SceneEditCommand::Type::SetTransform; })) {
        // Record inverses BEFORE applying — recordBatch reads current scene
        // state to compute the reverse for each Authoring cmd. Preview/replay
        // cmds are skipped inside recordBatch.
        m_undoStack.recordBatch(pendingEdits, usdScene);

        // Dedup by prim — for a 1000Hz mouse @ 60Hz frame we get ~16 motion events
        // per frame all targeting the same prim; only the latest matters.
        std::unordered_map<uint32_t, const SceneEditCommand*> latest;
        for (const auto& cmd : pendingEdits) {
            latest[cmd.prim.index] = &cmd;
        }

        std::vector<PrimHandle> dirty;
        for (auto& [_, cmd] : latest) {
            usdScene.setTransform(cmd->prim, cmd->transform, {.purpose = cmd->purpose});
            appendSubtree(usdScene, cmd->prim, dirty);

            // Name the observation by the USD prim path — stable across runs,
            // unlike PrimHandle::index (see §5.4).
            const auto* rec = usdScene.getPrimRecord(cmd->prim);
            OBS_EVENT("Scene", "PrimTransformed", rec != nullptr ? rec->path : std::string{}).field("path", "fast");
        }
        pendingEdits.clear();

        usdExtractor.patchTransforms(usdScene, meshLib, dirty, renderWorld);
        sceneQuery.updateDirty(usdScene, meshLib, dirty, usdScene.frameIndex());
        // Promote None -> TransformsOnly; preserve Full from a Phase 1 swap above.
        auto finalResult = result == SceneUpdateResult::Full ? SceneUpdateResult::Full : SceneUpdateResult::TransformsOnly;
        OBS_EVENT("Scene", "SystemExecuted", "SceneUpdater")
            .field("edits", (int64_t) latest.size())
            .field("path", "fast")
            .field("result", resultToString(finalResult));
        return finalResult;
    }

    // Phase 2: Kick off background job if edits are pending
    if (!editingBlocked && !pendingEdits.empty()) {
        // Record inverses while we still have the pre-edit scene state on the
        // main thread; the job will mutate USD asynchronously.
        m_undoStack.recordBatch(pendingEdits, usdScene);

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
                    case SceneEditCommand::Type::CreatePrim:
                        usdScene.createPrim(cmd.parentPath.c_str(), cmd.primName.c_str(), cmd.typeName.c_str(), {.purpose = cmd.purpose});
                        break;
                    case SceneEditCommand::Type::CreateReferencePrim:
                        usdScene.createReferencePrim(cmd.parentPath.c_str(), cmd.primName.c_str(), cmd.referenceAsset.c_str(), {.purpose = cmd.purpose});
                        break;
                    case SceneEditCommand::Type::SetDisplayColor:
                        usdScene.setDisplayColor(cmd.prim, cmd.colorValue, {.purpose = cmd.purpose});
                        break;
                    case SceneEditCommand::Type::RemovePrim: {
                        // Undo-replay of a create edit arrives with `prim` unset — look it up
                        // by path. User-initiated deletes arrive with a live handle.
                        auto h = cmd.prim;
                        if (!h && !cmd.parentPath.empty() && !cmd.primName.empty()) {
                            auto fullPath = cmd.parentPath + "/" + cmd.primName;
                            h = usdScene.findPrim(fullPath.c_str());
                        }
                        if (h) {
                            usdScene.removePrim(h, {.purpose = cmd.purpose});
                        }
                        break;
                    }
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

    // Phase 3: Drain USD notices when nothing is queued/in-flight. Most notices
    // here are deferred follow-ups to fast-path Authoring commits — we don't want
    // to redo a full extract + BVH rebuild for those. Branch on the dirty kind:
    //   • primsResynced → hierarchy changed: full asset rebuild + extract.
    //   • assetsDirty   → visibility / material change: full extract (libs unchanged).
    //   • transformDirty only → incremental patch (mirrors the fast path).
    if (!editingBlocked) {
        usdScene.beginFrame();
        usdScene.processChanges();
        usdScene.endFrame();

        const auto& dirty = usdScene.dirtySet();
        bool needsAssetRebuild = !dirty.primsResynced.empty();
        bool needsFullExtract = needsAssetRebuild || !dirty.assetsDirty.empty();

        if (needsAssetRebuild) {
            usdScene.updateAssetBindings(meshLib, matLib);
        }
        if (needsFullExtract) {
            usdExtractor.extract(usdScene, meshLib, renderWorld);
            sceneQuery.rebuild(usdScene, meshLib);
            // Only flag Full when libraries actually changed (resync), so the caller's
            // lib shared_ptr cache isn't spuriously invalidated by visibility flips etc.
            result = needsAssetRebuild ? SceneUpdateResult::Full : SceneUpdateResult::TransformsOnly;
        } else if (!dirty.transformDirty.empty()) {
            std::vector<PrimHandle> dirtyExpanded;
            for (auto h : dirty.transformDirty) {
                appendSubtree(usdScene, h, dirtyExpanded);
            }
            usdExtractor.patchTransforms(usdScene, meshLib, dirtyExpanded, renderWorld);
            sceneQuery.updateDirty(usdScene, meshLib, dirtyExpanded, usdScene.frameIndex());
            if (result != SceneUpdateResult::Full) {
                result = SceneUpdateResult::TransformsOnly;
            }
        }
    }

    // Silent frames (result == None) are the common case — don't spam an
    // observation for every tick that did nothing. Only narrate ticks that
    // actually changed something.
    if (result != SceneUpdateResult::None) {
        OBS_EVENT("Scene", "SystemExecuted", "SceneUpdater").field("path", "async").field("result", resultToString(result));
    }

    return result;
}

auto SceneUpdater::waitIfBlocked() -> void {
    if (editingBlocked) {
        JobSystem::wait(sceneUpdateFence);
        editingBlocked = false;
    }
}
