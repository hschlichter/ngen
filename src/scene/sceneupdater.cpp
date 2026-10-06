#include "sceneupdater.h"
#include "profile.h"

#include "trace.h"
#include "usdrenderextractor.h"
#include "usdscene.h"

#include <algorithm>
#include <format>
#include <unordered_map>

namespace {
auto editTypeName(SceneEditCommand::Type type) -> const char* {
    switch (type) {
        case SceneEditCommand::Type::MuteLayer:
            return "MuteLayer";
        case SceneEditCommand::Type::SetTransform:
            return "SetTransform";
        case SceneEditCommand::Type::SetVisibility:
            return "SetVisibility";
        case SceneEditCommand::Type::AddSubLayer:
            return "AddSubLayer";
        case SceneEditCommand::Type::ClearSession:
            return "ClearSession";
        case SceneEditCommand::Type::CreatePrim:
            return "CreatePrim";
        case SceneEditCommand::Type::CreateReferencePrim:
            return "CreateReferencePrim";
        case SceneEditCommand::Type::RemovePrim:
            return "RemovePrim";
        case SceneEditCommand::Type::SetDisplayColor:
            return "SetDisplayColor";
    }
    return "Unknown";
}

// One applied edit, named by the prim path it touches. Preview edits arrive every frame while a gizmo drags and only
// update the runtime cache; the Authoring edit that commits the drag is the one traced.
auto traceEdit(const USDScene& usdScene, const SceneEditCommand& cmd) -> void {
    if (cmd.purpose == SceneEditRequestContext::Purpose::Preview) {
        return;
    }
    std::string subject;
    if (cmd.prim) {
        if (const auto* rec = usdScene.getPrimRecord(cmd.prim)) {
            subject = rec->path;
        }
    } else if (!cmd.parentPath.empty()) {
        subject = cmd.parentPath + "/" + cmd.primName;
    } else {
        subject = cmd.stringValue;
    }
    const auto* name = editTypeName(cmd.type);
    TRACE_EVENT("Scene", "EditApplied", subject).text(std::format("{}{}", name, cmd.fromHistory ? " (undo/redo)" : "")).field("edit", name).field("from_history", cmd.fromHistory);
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
        PROFILE_ZONE("SwapJobResults");
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
            traceEdit(usdScene, *cmd);
        }
        pendingEdits.clear();

        {
            PROFILE_ZONE("PatchTransforms");
            PROFILE_ZONE_VALUE(dirty.size());
            usdExtractor.patchTransforms(usdScene, meshLib, dirty, renderWorld);
            sceneQuery.updateDirty(usdScene, meshLib, dirty, usdScene.frameIndex());
        }
        // Promote None -> TransformsOnly; preserve Full from a Phase 1 swap above.
        return result == SceneUpdateResult::Full ? SceneUpdateResult::Full : SceneUpdateResult::TransformsOnly;
    }

    // Phase 2: Kick off background job if edits are pending
    if (!editingBlocked && !pendingEdits.empty()) {
        // Record inverses while we still have the pre-edit scene state on the
        // main thread; the job will mutate USD asynchronously.
        m_undoStack.recordBatch(pendingEdits, usdScene);
        for (const auto& cmd : pendingEdits) {
            traceEdit(usdScene, cmd);
        }

        editingBlocked = true;
        pendingMeshLib = meshLib;
        pendingMatLib = matLib;
        auto edits = std::move(pendingEdits);
        pendingEdits.clear();

        sceneUpdateFence = JobSystem::submit(
            [&usdScene,
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
            },
            "SceneUpdateJob");
    }

    // Phase 3: Drain USD notices when nothing is queued/in-flight. Most notices
    // here are deferred follow-ups to fast-path Authoring commits — we don't want
    // to redo a full extract + BVH rebuild for those. Branch on the dirty kind:
    //   • primsResynced → hierarchy changed: full asset rebuild + extract.
    //   • assetsDirty   → visibility / material change: full extract (libs unchanged).
    //   • transformDirty only → incremental patch (mirrors the fast path).
    if (!editingBlocked) {
        {
            PROFILE_ZONE("UsdProcessChanges");
            usdScene.beginFrame();
            usdScene.processChanges();
            usdScene.endFrame();
        }

        const auto& dirty = usdScene.dirtySet();
        bool needsAssetRebuild = !dirty.primsResynced.empty();
        bool needsFullExtract = needsAssetRebuild || !dirty.assetsDirty.empty();

        if (needsAssetRebuild) {
            PROFILE_ZONE("UpdateAssetBindings");
            usdScene.updateAssetBindings(meshLib, matLib);
        }
        if (needsFullExtract) {
            {
                PROFILE_ZONE("Extract");
                usdExtractor.extract(usdScene, meshLib, renderWorld);
            }
            {
                PROFILE_ZONE("QueryRebuild");
                sceneQuery.rebuild(usdScene, meshLib);
            }
            // Only flag Full when libraries actually changed (resync), so the caller's
            // lib shared_ptr cache isn't spuriously invalidated by visibility flips etc.
            result = needsAssetRebuild ? SceneUpdateResult::Full : SceneUpdateResult::TransformsOnly;
        } else if (!dirty.transformDirty.empty()) {
            std::vector<PrimHandle> dirtyExpanded;
            for (auto h : dirty.transformDirty) {
                appendSubtree(usdScene, h, dirtyExpanded);
            }
            {
                PROFILE_ZONE("PatchTransforms");
                PROFILE_ZONE_VALUE(dirtyExpanded.size());
                usdExtractor.patchTransforms(usdScene, meshLib, dirtyExpanded, renderWorld);
                sceneQuery.updateDirty(usdScene, meshLib, dirtyExpanded, usdScene.frameIndex());
            }
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
