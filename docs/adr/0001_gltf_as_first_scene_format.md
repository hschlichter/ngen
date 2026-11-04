# 1. glTF (cgltf) as the first scene format

Date: 2026-03-31

## Status

Superseded by [ADR 3](0003_openusd_vendored_not_tinyusdz.md) and [ADR 6](0006_usd_stage_is_the_scene.md)

## Context

ngen began as a step-by-step Vulkan learning plan: vertex and index buffers, staging, uniform buffers, depth, textures, and then "Step 8: GLTF
Loading" and "Step 9: Scene Graph". The aim was to get real, textured, multi-mesh models on screen with as little machinery as possible. The engine
had no scene system yet; the renderer drew whatever list of meshes it was handed.

## Decision

Load glTF files with the single-header cgltf parser, walk the node tree accumulating transforms, and hand the renderer a flat list of meshes. There is
no engine scene model behind the loader.

## Consequences

- Textured multi-mesh scenes appeared on screen quickly, with one small dependency.
- There was no layering, composition, payloads or variants — nothing a layered scene format would bring.
- glTF was always the intermediate step: the USD plans that followed called it intermediate, and it was removed a week later (2026-04-08, "Removed
  GLTF") when the scene moved to OpenUSD.

## Verification

- Sources: [GLTF_PLAN.md](../GLTF_PLAN.md), [plan_usd_scene_system_implementation.md](../plan_usd_scene_system_implementation.md) §1,
  [docs/README.md](../README.md) ("historical — glTF was the intermediate format, replaced by USD").
- Code: no glTF loading code is left in `src/`.
- Differs from plan:
  - The `external/cgltf` submodule is still in `.gitmodules`, and `build.cpp:461` still passes `external/cgltf` as an include path for ngen-view.
    Both are leftovers from this decision.
