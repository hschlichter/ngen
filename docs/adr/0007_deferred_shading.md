# 7. Deferred shading: G-buffer and a lighting pass

Date: 2026-04-12

## Status

Accepted

## Context

The renderer had a single forward pass, with one hard-coded light computed inline in the fragment shader. More lights, debug views of intermediate
buffers and post-processing were wanted, while the overlays and the UI should stay as they were. The new frame graph
([ADR 5](0005_frame_graph.md)) made adding passes and transient targets cheap.

## Decision

- A geometry pass writes albedo and normals to multiple render targets, plus depth.
- A fullscreen lighting pass reads them as frame-graph transients and shades one directional light plus ambient.
- Overlays and the UI draw on the backbuffer after anti-aliasing, so they are never filtered.

The first plan (v1) was the same split with less detail; v2 is the one that landed. No forward+ or clustered alternative was discussed.

## Consequences

- G-buffer debug views come almost for free, and lighting cost no longer depends on overdraw.
- An instance-id target in the G-buffer later made picking a GPU readback.
- More memory bandwidth per pixel; later trimmed by a 16-bit normal target.
- Still only one directional light: multiple UsdLux lights are planned (`plan_usd_lights.md`) but not built.
- Translucency has no path; only cutout materials are supported ([ADR 56](0056_cutout_buckets.md)).

## Verification

- Sources: [basic_lighting_pass_plan.md](../basic_lighting_pass_plan.md), [basic_lighting_pass_plan_v2.md](../basic_lighting_pass_plan_v2.md),
  [plan_geometry_pass_cost.md](../plan_geometry_pass_cost.md), [plan_compute_aa_pass.md](../plan_compute_aa_pass.md).
- Code:
  - `src/renderer/passes/geometrypass.cpp:33`: colour formats `{R8G8B8A8_UNORM, normalTargetFormat, R32_UINT}`; `:24-26` choose a 16-bit float
    normal target, falling back to 32-bit with a warning.
  - `src/renderer/passes/lightingpass.cpp:106-109`: the lighting pass writes a scene-colour target.
  - `src/renderer/passes/aapass.cpp:33-39`: anti-aliasing is a compute pass (FXAA) after lighting.
- Differs from plan:
  - v2 wrote the lit result to the swapchain; it now goes to a float scene-colour target, then compute FXAA, then a blit.
  - Normals moved from 32-bit to 16-bit float, and an `R32_UINT` instance-id target was added.
