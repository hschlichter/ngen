# 58. `ngen-editor` owns USD; changes reach views as deltas

Date: 2026-10-13

## Status

Proposed

From [plan_editor_split.md](../plan_editor_split.md), still a draft. Only its precondition holds: the editor code sits unchanged and unbuilt in
`src/editor/`. The roadmap (2026-10-14/15) narrowed the first step to a minimal editor without deltas; see Verification.

## Context

Once ngen-view renders from layer packs and links no OpenUSD ([ADR 52](0052_view_renders_layer_packs.md)), the view can no longer edit USD. The
layer browser, undo stack, prim creation and gizmo authoring that lived in the view ([ADR 6](0006_usd_stage_is_the_scene.md),
[ADR 9](0009_preview_vs_authoring_edits.md)) have no program. Editing needs an owner of the USD stage with layers, undo and save, and several views
should be able to follow an edit without repacking the whole scene every frame.

The process split ([ADR 34](0034_process_split.md)) already names that owner: `ngen-editor`, a separate process that holds the stage.

## Decision

Proposed in the plan:

- `ngen-editor` (`src/apps/editor.cpp` plus `src/editor/`, no renderer) owns the USD stage and undo. Unsaved edits stay in memory until
  `scene.save`.
- Views keep applying gizmo drags as local opinions (preview) and send `prim.setTransform` in preview and authoring forms, tagged with an `editSeq`.
- The editor sends views deltas against a manifest, with a base version and a resync when a view falls behind.
- The editor maps entities to prims and owns the selection.
- The editor starts its own viewport (`ngen-view --editor`).
- How an unsaved edit becomes packed data is left open; an in-memory packing fast path is ruled out, because only the asset server packs
  ([ADR 42](0042_separate_asset_server.md)).

## Consequences

- Easier: the OpenUSD boundary is clean; the view stays free of pxr while editing still exists.
- Easier: several views (or agents) can follow one editor.
- Harder: deltas, a manifest and version tracking are a protocol to design and keep in step with the World's types.
- Harder: until it is built, no edit is authored or saved anywhere; a view gizmo edit is lost when the view closes.
- Harder: the code in `src/editor/` has drifted from the types it used, so building it starts with bringing it up to date.

## Verification

- Sources: [plan_editor_split.md](../plan_editor_split.md), [plan_tool_architecture.md](../plan_tool_architecture.md) (step 3, "Two edit paths",
  "Deltas"), [plan_scene_pack.md](../plan_scene_pack.md) (decisions 6, 14), [roadmap.md](../roadmap.md) (vertical slice step 3).
- Code: `src/editor/README.md:1-5` (unbuilt, nothing in `build.cpp` compiles it); `src/editor/README.md:19-21` (it has drifted); no `editor.cpp` in
  `src/apps/`; no `editSeq` or delta types in `src/world`, `src/view` or `src/renderer`.
- Differs from plan:
  - The roadmap's vertical slice step 3 (decided 2026-10-14) scopes a minimal editor first: open the stage, select, transform and save to a layer,
    with the view picking the edit up through asset change notices ([ADR 57](0057_asset_change_notices.md)). No deltas, live unsaved edits or
    cross-process undo in that step.
  - The plan's phase A (deltas with the editor still inside the view) was overtaken: the scene pack work removed the in-view editor first.
  - The plan's "Current state" still describes the editor UI living in ngen-view (`src/ui/`, `src/main.cpp`), and it still names a build server.
