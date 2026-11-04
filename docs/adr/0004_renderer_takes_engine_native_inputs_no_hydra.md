# 4. The renderer takes engine-native flat inputs, no Hydra

Date: 2026-04-02

## Status

Accepted

## Context

ngen already had its own renderer, frame graph and Vulkan RHI when USD arrived. USD's own rendering architecture, Hydra (scene delegate → render
index → render delegate), overlaps with all three and would impose Hydra's model of rendering on the engine. Hydra's value is mostly in viewports
inside content-creation tools.

The question came back when the view moved from a USD stage to scene packs ([ADR 52](0052_view_renders_layer_packs.md)): should the renderer walk
the runtime World directly?

## Decision

- A render-extraction step turns the scene into plain engine types — `RenderWorld`, `MeshLibrary`, `MaterialLibrary` and incremental `SceneChanges` —
  and the renderer knows nothing about where they came from.
- From Hydra, only the ideas of dirty tracking and categorised prims are borrowed.
- After the switch to packs, the view (`ViewScene`, and later its placement tree) does the flattening that `USDRenderExtractor` used to do. The
  renderer was not changed by the switch.

Alternatives considered:

- Hydra with a custom render delegate: rejected for the overlap and the foreign model.
- The renderer reading the World's entities directly: rejected when the packs landed, because it would have changed the renderer in the same step as
  the scene source.

## Consequences

- The scene source could be swapped — USD stage to scene packs — without touching the renderer.
- There is an extraction (later placement) layer to maintain between the scene and the renderer.
- Instance expansion, LOD choice and purpose choice live in the view, not in the renderer.

## Verification

- Sources: [usd_scene_system_status.md](../usd_scene_system_status.md) §1 "Why Not Hydra",
  [plan_usd_scene_system_implementation.md](../plan_usd_scene_system_implementation.md) §1,
  [usd_driven_scene_system_architecture_modern_c_engine.md](../usd_driven_scene_system_architecture_modern_c_engine.md) §22.3 and §25,
  [plan_scene_pack.md](../plan_scene_pack.md) decision 22.
- Code:
  - `src/renderer/renderworld.h:128` (`RenderLight`), `:176` (`RenderWorld`), `:184` (`SceneChanges`).
  - [src/scene/README.md](../../src/scene/README.md): the shared plain types, with no USD and no runtime scene.
  - `build.cpp:173-174`: the `world` library is "No OpenUSD"; the renderer library links neither the World nor pxr.
