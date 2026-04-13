#include "sceneupdater.h"

#include "usdrenderextractor.h"
#include "usdscene.h"

#include <algorithm>
#include <unordered_map>

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
    if (!editingBlocked && !pendingEdits.empty()
        && std::ranges::all_of(pendingEdits, [](const auto& e) { return e.type == SceneEditCommand::Type::SetTransform; })) {
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
        }
        pendingEdits.clear();

        usdExtractor.patchTransforms(usdScene, meshLib, dirty, renderWorld);
        sceneQuery.updateDirty(usdScene, meshLib, dirty, usdScene.frameIndex());
        // Promote None -> TransformsOnly; preserve Full from a Phase 1 swap above.
        return result == SceneUpdateResult::Full ? SceneUpdateResult::Full : SceneUpdateResult::TransformsOnly;
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

    return result;
}

auto SceneUpdater::waitIfBlocked() -> void {
    if (editingBlocked) {
        JobSystem::wait(sceneUpdateFence);
        editingBlocked = false;
    }
}
