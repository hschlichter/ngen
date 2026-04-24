# Incremental Scene Updates for Transform Edits

## Context

Today every `SetTransform` edit (Properties panel, translate gizmo) causes the entire scene pipeline to re-run on a background thread. Concretely,
`SceneUpdater::update` (`src/scene/sceneupdater.cpp:23-68`) submits an async job that:

1. Applies the edit(s) to USD,
2. Calls `usdExtractor.extract()` — **clears `RenderWorld` and re-walks every prim** (`src/scene/usdrenderextractor.cpp:7-30`),
3. Calls `sceneQuery.rebuild()` — **rebuilds bounds cache + BVH from scratch** (`src/scene/scenequery.cpp:5-8`),
4. Sets `editingBlocked = true` so further edits queue up until the job completes and Phase 1 swaps the results in.

For a translate-gizmo drag, this means: every motion event queues an edit; only the first edit per "round trip" actually triggers work; further edits sit in
`pendingEdits` until the previous job's results swap in. User sees the prim trail behind the cursor by the round-trip cost (extract + BVH rebuild +
descriptor-set rebuild on render thread, even when only one transform changed).

The good news: `Renderer::uploadRenderWorld` (`src/renderer/renderer.cpp:111-150`) already short-circuits when only transforms change — same instance count,
same mesh/material per slot → it just copies the new transforms into `gpuInstances` and returns without touching GPU descriptor sets or calling `waitIdle()`.
The transforms are pushed per-draw via push constants (`src/renderer/passes/geometrypass.cpp:125-126`), so updating the CPU vector is enough. The bottleneck is
**upstream** — in `SceneUpdater` and `USDRenderExtractor`.

Better news: the incremental machinery already exists but is dead code:
- `SceneQuerySystem::updateDirty` (`src/scene/scenequery.cpp:10-13`) — calls `BoundsCache::updateDirty` and `SpatialIndex::refit`. **Never called.**
- `BoundsCache::updateDirty` (`src/scene/boundscache.cpp:50-71`) — patches only the listed prims.
- `SpatialIndex::refit` — walks existing tree and recomputes node AABBs from leaves (no rebuild).
- `USDScene::dirtySet().transformDirty` (`src/scene/usdscene.cpp:443-464`) — already populated by `processChanges()` from USD notices.
- `USDScene::setTransform` (`src/scene/usdscene.cpp:945-996`) already updates the local transform cache for the prim **and its descendants** synchronously,
  before any notice fires.

## Goal

A `SetTransform` edit should patch only the affected instances and BVH nodes on the **main thread, synchronously, in microseconds**, without involving the async
job queue or rebuilding any descriptor state. Other edits (visibility, layer mute, asset changes, resyncs) keep using the existing async batch path — they're
rare and genuinely heavy.

## Design

### 1. Track prim → instance index in `RenderWorld`

`USDRenderExtractor::extract` currently produces a flat `meshInstances` array with no link back to the source prim. To patch a specific prim's instance we need
a reverse map.

**Modify `src/renderer/renderworld.h`:**
- Add `std::vector<PrimHandle> instancePrim` parallel to `meshInstances` (extractor sets one entry per pushed instance).
- Add `std::unordered_map<uint32_t, uint32_t> primToInstance` (prim index → instance index). Populated by extractor; cleared by `RenderWorld::clear()`.

(`PrimHandle` is already in `src/scene/scenehandles.h`; `RenderWorld` already includes `scenetypes.h` which transitively gets it via `scenehandles.h` — verify
`#include "scenehandles.h"` lands.)

### 2. Add `USDRenderExtractor::patchTransforms`

**Modify `src/scene/usdrenderextractor.{h,cpp}`:**

```cpp
auto USDRenderExtractor::patchTransforms(const USDScene& scene, const MeshLibrary& meshLib,
                                         std::span<const PrimHandle> dirty, RenderWorld& out) -> void {
    for (auto h : dirty) {
        auto it = out.primToInstance.find(h.index);
        if (it == out.primToInstance.end()) continue; // not renderable, or hidden — nothing to patch
        const auto* xf = scene.getTransform(h);
        const auto* binding = scene.getAssetBinding(h);
        if (!xf || !binding || !binding->mesh) continue;
        auto& inst = out.meshInstances[it->second];
        inst.worldTransform = xf->world;
        inst.worldBounds = meshLib.bounds(binding->mesh).transformed(xf->world);
    }
}
```

Also update `extract()` to populate the new `instancePrim` and `primToInstance` fields alongside each `meshInstances.push_back`.

### 3. Fast-path branch in `SceneUpdater::update`

**Modify `src/scene/sceneupdater.cpp`:**

After Phase 1 (swap completed async results) and before Phase 2 (submit async job), insert a synchronous fast path:

```cpp
// Fast path: if all pending edits are SetTransform AND we're not currently
// blocked by a background job, apply them inline. Avoids the round-trip cost
// of the async pipeline for the common interactive case (gizmo drag, Properties
// scrubbing). Other edit types fall through to Phase 2 as before.
if (!editingBlocked && !pendingEdits.empty()) {
    bool allTransforms = std::ranges::all_of(pendingEdits,
        [](const auto& e) { return e.type == SceneEditCommand::Type::SetTransform; });
    if (allTransforms) {
        for (const auto& cmd : pendingEdits) {
            usdScene.setTransform(cmd.prim, cmd.transform);
        }
        pendingEdits.clear();

        usdScene.beginFrame();
        usdScene.processChanges();
        usdScene.endFrame();

        // Expand transformDirty to include descendants. setTransform() already
        // refreshed descendants in the local transform cache, but the USD notice
        // only fires for the directly-edited prim; we need both for downstream patching.
        std::vector<PrimHandle> dirty = expandTransformDirty(usdScene, usdScene.dirtySet().transformDirty);

        usdExtractor.patchTransforms(usdScene, meshLib, dirty, renderWorld);
        sceneQuery.updateDirty(usdScene, meshLib, dirty, usdScene.frameIndex());
        return true;
    }
}
```

`expandTransformDirty` walks each dirty prim's subtree using `USDScene::firstChild` / `nextSibling` (already in `src/scene/usdscene.h:122-123`) and returns the
flat list. Implement as a free function in `sceneupdater.cpp` — small, single-use.

### 4. Render-thread side: nothing to do

`Renderer::uploadRenderWorld` (`src/renderer/renderer.cpp:117-150`) already detects "only transforms differ" and exits at line 150 after copying transforms into
`gpuInstances`. The geometry pass reads transforms via push-constants per draw (`src/renderer/passes/geometrypass.cpp:125-126`), so no GPU resource rebuild, no
`waitIdle()`. The full `RenderWorld` is still copied into `RenderUpload` (~tens of bytes per instance), which is cheap and keeps the contract simple.

Confirm by inspection: lines 132-150 short-circuit when meshes/materials are unchanged — they still hit the `gpuInstances.resize` + per-instance copy at lines
142-146 and the `if (!geometryChanged) return;` at 148. ✓

## Files to modify

| File | Change |
|---|---|
| `src/renderer/renderworld.h` | Add `instancePrim` vector + `primToInstance` map; update `clear()` |
| `src/scene/usdrenderextractor.h` | Declare `patchTransforms` |
| `src/scene/usdrenderextractor.cpp` | Populate prim-index map in `extract()`; add `patchTransforms` |
| `src/scene/sceneupdater.cpp` | Fast-path branch in `update()`; `expandTransformDirty` helper |

No header surface change to `SceneUpdater`. No changes to `Renderer`, `RenderThread`, or `RenderSnapshot`. No changes to the async batch path.

## Out of scope

- Async path for non-transform edits — leave as-is. Visibility/asset/layer changes are infrequent and the existing batch is fine.
- Coalescing the per-frame `submitRenderUpload` copy — `RenderWorld` is small enough to copy; revisit only if a profile shows it.
- Multi-edit batching beyond a single frame.
- Undo/redo plumbing (orthogonal).

## Verification

1. `bear -- make` builds clean.
2. Open a USD scene; drag the translate gizmo on a prim — motion is 1:1 with cursor (no trailing).
3. Confirm Properties panel still updates live during drag.
4. With Tracy/printf timers around `SceneUpdater::update`, confirm fast path runs in <1 ms for a single-prim drag (vs. previous round-trip).
5. Click "Visible" toggle in Properties — falls through to async path; behavior unchanged.
6. Edit a Sublayer mute toggle — async path unchanged.
7. Resync (open a new scene, switch edit target with new prims) — async path unchanged.
8. Verify `sceneQuery.raycast` still returns correct hits after a drag (proves BVH refit works) — click the moved prim again.
9. Drag a parent of a hierarchy and verify children also move on screen and remain pickable (proves descendant expansion works).
10. `make format` — no diff.
