# 9. Preview vs authoring edits

Date: 2026-04-13

## Status

Accepted

Amended by [ADR 52](0052_view_renders_layer_packs.md): the view no longer edits USD; only the preview half exists in the built view, as a local
opinion. The authoring half waits for the editor process ([ADR 58](0058_editor_process_owns_usd.md)).

## Context

With the USD stage as the scene ([ADR 6](0006_usd_stage_is_the_scene.md)), a gizmo drag wrote a USD layer on every mouse-motion event: an `Sdf`
write, an edit context, a change notice and a drain of the dirty set. That stalled frames. The design note put it plainly: USD is a content authoring
format, not a runtime transform store. Heavy edits such as muting a layer already went through an asynchronous copy-and-swap batch on the job system.

## Decision

- Every interactive edit has a **preview** phase: an in-memory transform cache, patches to the render instances and bounding-volume refits, with no
  layer write.
- It ends with one **authoring** commit at the operation boundary (mouse release, or an ImGui widget's "deactivated after edit").
- Undo records inverse commands for authoring edits only, one undo step per operation, kept in memory.
- Preview state is never saved.

Alternatives considered:

- Authoring on every frame of the drag: rejected — it is the stall this decision removes.

## Consequences

- Drags are cheap, and the operation boundary is a natural undo step.
- The model carried forward into the process split: a view applies a gizmo drag as a local opinion, and the editor authors it on release
  ([ADR 34](0034_process_split.md)).
- Today, with no editor process, a gizmo edit in the view is lost when the view closes.
- The Properties panel still authored on every frame, an exception the plan admitted.

## Verification

- Sources: [architecture_preview_vs_authoring.md](../architecture_preview_vs_authoring.md),
  [plan_incremental_transform_updates.md](../plan_incremental_transform_updates.md), [plan_undo_redo.md](../plan_undo_redo.md),
  [plan_translate_gizmo.md](../plan_translate_gizmo.md), [plan_rotate_gizmo.md](../plan_rotate_gizmo.md),
  [plan_background_scene_updates.md](../plan_background_scene_updates.md), [plan_editor_split.md](../plan_editor_split.md) decision 2.
- Code:
  - Unbuilt editor code: `src/editor/scene/usdscene.h:79-84` (`Purpose { Authoring, Preview, ... }`), `src/editor/scene/sceneupdater.h:23`,
    `src/editor/scene/undostack.h:22`.
  - Built view: `src/view/viewscene.h:330` (`setLocalTransform`), `:726` (`localOverrides`, kept until the view closes);
    `src/tools/toolshooks.cpp:91` and `:97` call it on drag and on release.
- Differs from plan:
  - The plans' paths (`src/scene/usdscene.cpp`, `src/main.cpp`, `src/ui/editcommand.h`) are stale; the code is in `src/editor/`, and `main.cpp`
    became `src/apps/view.cpp`.
