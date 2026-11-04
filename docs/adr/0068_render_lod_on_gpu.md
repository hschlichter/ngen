# 68. Render LOD: inline levels picked on the GPU, payload levels stream

Date: 2026-10-19

## Status

Accepted

Amends [ADR 50](0050_usdlod_only_lod_convention.md): not every level is a payload; what is authored decides how a root is handled.

## Context

At the test world's `dense_forest`, only 17 of 42,387 LOD roots had payload levels. The rest were per-prop asset levels referenced from one file.
Handling those as streaming roots, with level switches as scene-graph edits, cost 200 to 1,000 milliseconds per pass. Props switch level every
frame as the camera moves, at tens of thousands of groups, and that has to happen without a CPU round-trip.

## Decision

- A root streams if any of its levels has a payload ([ADR 70](0070_residency_from_streaming_radii.md)). Otherwise every level is composed in place
  and the root becomes a `RenderLodGroup`.
- Every level's draws get their own instance slots, tagged with their group and level.
- A `LodSelect` compute pass, one thread per group, runs before culling: it applies forced levels, the heuristic with hysteresis or blend band
  ([ADR 69](0069_lod_heuristics_and_dither_fade.md)), and the residency fallback, and writes a per-group state the cull pass reads to drop the slots
  of levels not drawn.
- The shadow cascades draw the camera's level. The live main camera decides, not a frozen frustum or the debug camera
  ([ADR 72](0072_main_camera_decides.md)).
- Up to 8 levels per group. A level that isn't resident falls back through a residency mask the cull pass returns, one frame late.
- The CPU and the GPU share one pick formula (`pickLodLevel`).

Rejected: one slot holding several levels' ranges (deferred until memory or cull time shows a need), picking per instance inside the cull pass
(many writers per state word), and a variable or 5-level record.

## Consequences

- Easier: camera moves change nothing on the CPU for props; there is no CPU in the LOD loop.
- Easier: CPU and GPU picks agree, so the LOD tool's inspector shows what the GPU does.
- Harder: every level of every placed asset holds slots, about three times the asset's draws.
- Harder: fading groups draw in the cutout buckets.

## Verification

- Sources: [plan_render_lod.md](../plan_render_lod.md), [plan_lod_blending.md](../plan_lod_blending.md),
  [plan_streaming_lod.md](../plan_streaming_lod.md) (decision 2, marked), `src/renderer/README.md` ("Render LOD").
- Code: `src/world/world.h:198-201` (`LodRoot::streams`); `src/renderer/renderworld.h:151-152` (`RenderLodGroup`, `maxLevels = 8`);
  `src/renderer/renderer.cpp:1074` (the `LodSelect` camera); `shaders/lodselect.comp`; `src/world/world.cpp:370` (`pickLodLevel`).
- Differs from plan: decision 8 sized the GPU group record at 64 bytes; `GpuLodGroup` is 240 bytes after the distance heuristic and blend
  thresholds were added (`src/renderer/lodgroups.h:39`).
