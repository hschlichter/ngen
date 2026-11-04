# 6. The USD stage is the scene in ngen-view (Model A)

Date: 2026-04-06

## Status

Superseded by [ADR 52](0052_view_renders_layer_packs.md)

Supersedes [ADR 1](0001_gltf_as_first_scene_format.md).

## Context

The early generic notes recommended an engine-native scene with USD as an import format: "USD describes content; the scene system owns live runtime
state". Henrik then made layering, references, payloads, variants and session layers first-class, which moved USD from the import edge to the core.
The design discussion set out two models: Model A, where the USD stage is authoritative, and Model B, where the engine keeps a full runtime mirror of
USD.

## Decision

The USD stage is the scene system in ngen-view, with no separate engine scene graph or ECS.

- `USDScene`, a pimpl facade, holds the layer manager, the stage, a `TfNotice` change listener feeding a dirty set, and caches for prims,
  transforms, bounds and asset bindings.
- A BVH-based scene query system does picking.
- `USDRenderExtractor` produces the renderer's inputs ([ADR 4](0004_renderer_takes_engine_native_inputs_no_hydra.md)).
- Edits go through an edit context to a chosen edit target layer, with undo on top ([ADR 9](0009_preview_vs_authoring_edits.md)).

Alternatives considered:

- USD as an import format into an engine-native scene: rejected, because it loses layering and edit routing.
- Model B, a full runtime mirror: called "the practical sweet spot" in the discussion, but the plans chose "USD is the scene" with caches.

## Consequences

- Full USD semantics and in-process editing: a layers window, undo, prim creation, gizmos.
- The view was tied to pxr in every build, and start-up was USD's: Sponza took 2.6 seconds to open the stage and 5.4 seconds to the first frame.
- Large scenes (Caldera: 63,700 layers) could not stream through a single stage. This pushed the split into an editor process and scene packs
  ([ADR 34](0034_process_split.md), [ADR 52](0052_view_renders_layer_packs.md)).

## Verification

- Sources: [usd_scene_format_integration_modern_c_engine.md](../usd_scene_format_integration_modern_c_engine.md),
  [scene_system_architecture_modern_c_engine.md](../scene_system_architecture_modern_c_engine.md),
  [usd_driven_scene_system_architecture_modern_c_engine.md](../usd_driven_scene_system_architecture_modern_c_engine.md),
  [plan_for_a_layer_aware_usd_scene_system_modern_c_engine.md](../plan_for_a_layer_aware_usd_scene_system_modern_c_engine.md),
  [plan_usd_scene_system_implementation.md](../plan_usd_scene_system_implementation.md), [usd_scene_system_status.md](../usd_scene_system_status.md),
  [plan_layer_browser.md](../plan_layer_browser.md), [plan_prim_creation.md](../plan_prim_creation.md),
  [plan_background_scene_updates.md](../plan_background_scene_updates.md).
- Code: the code survives, unbuilt, in `src/editor/scene/` (`usdscene.h:90`, `usdrenderextractor.*`, `scenequery.*`, `spatialindex.*`,
  `boundscache.*`, `sceneupdater.*`, `undostack.*`) and `src/editor/ui/`. [src/editor/README.md](../../src/editor/README.md) says nothing in
  `build.cpp` compiles it.
- Differs from plan:
  - [docs/README.md](../README.md) labels the layer-aware plan "Model A", but that plan's own core principle (engine systems mostly work on
    synchronised runtime caches) reads closer to Model B.
  - `usd_scene_system_status.md` still describes `src/scene/usd*.cpp`; that code moved to `src/editor/scene/`.
