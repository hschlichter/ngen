# 30. Cascaded shadows in one atlas with hardware compare

Date: 2026-09-25

## Status

Accepted

Amended by [ADR 29](0029_gpu_culling_ordered_compaction.md): each cascade is a view of the GPU culling, not a CPU mask.

Amended by [ADR 55](0055_reversed_z_infinite_far.md): cascades fit up to a shadow distance (200 metres by default), not to the whole scene.

## Context

One 2048 by 2048 shadow map was fitted to the scene's bounding box: about one and a half centimetres per texel on Sponza, and far coarser on larger
scenes. The lookup was a single hard compare with no filtering. The RHI's `compareEnable` sampler field was plumbed through but unused.

## Decision

- N orthographic cascades with a practical split (lambda 0.5 between uniform and logarithmic), each fitted to the bounding sphere of its slice of the
  view and snapped to texels, so edges don't shimmer as the camera moves.
- The cascades are tiles of one depth atlas, drawn with a viewport and scissor per tile.
- Three cascades of 1,024 texels by default, after measuring 5.0 milliseconds for three against 6.1 for four.
- A `sampler2DShadow` hardware-compare sampler with 3 by 3 PCF, and slope and normal bias in the shader.

Rejected: a texture array (needs per-layer attachment views nothing else wants). Deferred, each with a trigger: raster depth bias in the RHI, blending
between cascades, variance or moment shadow maps, spot and point light shadows.

## Consequences

- Easier: no RHI change; stable, soft shadow edges.
- Harder: building-sized meshes land in every cascade, so each cascade costs about one scene draw.
- Harder: the extra passes hit the limit of 128 GPU zones per command buffer, which led to zones per draw region (ADR 26).

## Verification

- Sources: [plan_shadow_cascades.md](../plan_shadow_cascades.md), [src/renderer/README.md](../../src/renderer/README.md) "Shadows".
- Code: `src/renderer/shadowcascades.h:20` (`count = 3`), `:21` (`tileSize = 1024`), `:22` (`splitLambda = 0.5`), `:24` (`maxDistance = 200`);
  `src/renderer/passes/shadowpass.cpp:171`; `src/renderer/renderer.cpp:69`–`76` (compare sampler, `LessOrEqual`).
