# Preview vs Authoring: Interactive Edit Architecture

## Principle

Every interactive edit has two distinct lifecycles:

1. **Preview** — runtime-only mutation. Touches the in-memory transform cache
   (and downstream render world / spatial query). Cheap, idempotent, fires per
   frame during the operation. **No USD layer write.**
2. **Authoring** — commits the final value to the USD layer. Fires once, at the
   *end* of the operation (drag release, slider edit-end, animation step end).
   This is what gets serialized, saved, and (eventually) recorded for undo.

USD is a content authoring format, not a runtime transform store. `Sdf` layer
writes, `UsdEditContext`, `TfNotice` and the change-listener drain are all built
for layer (2). They must not be exercised every mouse-motion event.

## Mechanism

`SceneEditRequestContext::Purpose` (`src/scene/usdscene.h`) already enumerates
`Authoring`, `Preview`, `Procedural`, `Debug`. `SceneEditCommand`
(`src/ui/editcommand.h`) carries this `purpose` through the edit queue.

`USDScene::setTransform` (`src/scene/usdscene.cpp`) branches on `purpose`:
- **Preview**: writes the new local Transform straight into
  `m_impl->transforms[h]`, recomputes world from `parent.world * local`, and
  refreshes descendants' world matrices. The USD stage stays untouched.
- **Authoring**: full `UsdEditContext` + `transformOp.Set()` write, then the
  same cache refresh.

`SceneUpdater::update` (`src/scene/sceneupdater.cpp`) routes both purposes
through the same fast path: dedupe pending edits per prim, apply, then patch
only the affected `RenderWorld` instances (`USDRenderExtractor::patchTransforms`)
and refit the affected BVH leaves (`SceneQuerySystem::updateDirty`). No
`processChanges`, no async batch, no library copies, no descriptor rebuilds.

The renderer side (`Renderer::uploadRenderWorld`) already short-circuits when
only transforms changed — same `gpuInstances`, no GPU sync, transforms reach
the GPU via per-draw push constants in `geometrypass.cpp`.

## Per-frame cost during an interactive operation

Per dragged prim:
- 1 in-memory Transform write
- 1 world-matrix multiply (+ N descendants)
- 1 `RenderMeshInstance` patch (+ N descendants)
- 1 BVH leaf refit (+ N descendants)

That's it. No USD activity at all until commit.

## How a tool author uses this

For any new interactive feature (rotate gizmo, scale gizmo, vertex sculpt,
material parameter scrub, animation scrubbing, drag-to-reorder hierarchy):

1. **Capture** the current authored state at operation start (drag begin,
   slider grab). Stash it in a small per-operation struct.
2. **Per-frame Preview** — emit `SceneEditCommand` with
   `purpose = Purpose::Preview` while the operation is in progress.
3. **Commit on end** — emit one `SceneEditCommand` with the default
   `purpose = Purpose::Authoring` at operation end. This is the
   undo-significant moment and the only USD write.

If your design answer to *"what's the commit boundary?"* is "every frame",
you've designed it wrong — find the natural operation boundary (button up,
edit-end, key release) and commit there.

## Properties panel example

ImGui's `DragFloat3` / `DragFloat` widgets expose the operation lifecycle:
- The widget is *active* while the user holds the mouse → emit Preview.
- `ImGui::IsItemDeactivatedAfterEdit()` returns true on the frame the value was
  released → emit Authoring with the final value.

(Currently the Properties panel only emits Authoring per-frame, which is fine
for type-in-a-number scrubbing of small scenes but should adopt this same
pattern for parity with the gizmo.)

## What this does NOT change

- The async batch path in `SceneUpdater` is preserved for genuinely heavy edits
  (`MuteLayer`, `SetVisibility`, `AddSubLayer`, `ClearSession`, resyncs). Those
  are rare and the existing copy-and-rebuild model is appropriate.
- Saving (`USDScene::saveLayer`) writes whatever's in the authored layer, which
  is the last committed state. Preview state is intentionally never saved —
  if the editor crashes mid-drag, the Preview is lost. That matches every
  mainstream DCC.

## Related code

| Component | File |
|---|---|
| Purpose enum | `src/scene/usdscene.h` (`SceneEditRequestContext`) |
| Edit command | `src/ui/editcommand.h` (`purpose` field) |
| Preview branch | `src/scene/usdscene.cpp` (`USDScene::setTransform`) |
| Fast path + dedup | `src/scene/sceneupdater.cpp` |
| Incremental patch | `src/scene/usdrenderextractor.cpp` (`patchTransforms`) |
| BVH refit | `src/scene/scenequery.cpp` (`updateDirty`) |
| Renderer short-circuit | `src/renderer/renderer.cpp` (`uploadRenderWorld`) |
| Translate gizmo wiring | `src/main.cpp` (motion → Preview, mouse-up → Authoring) |
