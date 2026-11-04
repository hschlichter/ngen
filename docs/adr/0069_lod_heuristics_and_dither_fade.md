# 69. Both LOD heuristics; dither cross-fade over the authored band

Date: 2026-10-25

## Status

Accepted

## Context

The engine read only UsdLod's screen-size heuristic. The test world authors both a distance and a screen-size heuristic per root, in either order,
with `blendThresholds` saying over what band two levels may blend. Level switches popped visibly. The renderer is deferred and opaque
([ADR 7](0007_deferred_shading.md)), with no forward or transparency path to blend in, and screenshots are compared byte for byte.

## Decision

- Both heuristics are read. A view-wide preference picks which one decides: distance first by default, then screen size, or the authored order.
- Render-LOD levels ([ADR 68](0068_render_lod_on_gpu.md)) cross-fade inside the authored blend band only: the two levels draw with complementary
  4×4 ordered (Bayer) dither in screen space, so every pixel is covered once.
- Fading draws go through the cutout buckets ([ADR 56](0056_cutout_buckets.md)), so the opaque pipelines keep early depth testing.
- A fade happens only when both levels are resident. Where no band is authored, the 20% hysteresis band still applies.
- There is no time-based fade. Streaming roots don't cross-fade: inside a band they want the finer level.

Rejected: alpha blending, geomorphing, a fixed-time fade or a time fade where nothing is authored (deferred), discard in the opaque pipelines (loses
early depth), and blue noise.

## Consequences

- Easier: fades are deterministic, so screenshots stay byte-identical between runs.
- Easier: no new pipelines.
- Harder: the cutout buckets need command capacity for every render-LOD instance.
- Harder: the dither pattern is visible while a fade lasts; there is no temporal anti-aliasing to hide it.

## Verification

- Sources: [plan_lod_blending.md](../plan_lod_blending.md), [plan_lod_tool.md](../plan_lod_tool.md) (bias, per-asset tuning),
  [plan_streaming_domain.md](../plan_streaming_domain.md) (decision 4).
- Code: `src/view/viewscene.h:242` (`LodPreference::Distance` by default); `src/world/world.cpp:241` (`chooseLodHeuristic`), `:370`
  (`pickLodLevel`); `shaders/lodfade.glsl:1-5`; `src/renderer/README.md` ("Render LOD").
