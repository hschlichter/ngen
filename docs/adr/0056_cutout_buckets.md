# 56. Cutout materials in their own buckets and pipelines

Date: 2026-10-13

## Status

Accepted

## Context

UsdPreviewSurface materials can author an opacity and an opacity threshold: below the threshold the surface is cut away. Sponza's decals drew as
opaque quads, and picking hit them. A `discard` anywhere in a fragment shader disables early depth testing for every draw using that pipeline, so
adding a per-material branch to the one geometry pipeline would slow every opaque draw.

## Decision

- Draw buckets become cull mode times cutout: four per view instead of two.
- Cutout pipelines for the geometry pass, depth prepass and shadow pass sample the opacity and discard below the threshold; the opaque pipelines
  never discard and keep early depth.
- The opacity source is packed with the material: an opacity texture in its own material slot, the channel, and the threshold, in the material
  record's spare words. Raw masks are re-encoded from linear to sRGB in the shader.
- A prim with any cutout submesh casts shadows per submesh, so the cut-away parts cast none.

## Consequences

- Easier: correct decals, foliage shadows and picking through cut-away surfaces.
- Easier: opaque draws keep early depth.
- Easier: the cutout buckets were later reused for LOD cross-fades ([ADR 69](0069_lod_heuristics_and_dither_fade.md)).
- Harder: twice the buckets and pipelines.
- Harder: still no translucency or alpha-to-coverage, and cutouts thin out at small mip levels.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_cutout_materials.md](../plan_cutout_materials.md), [src/renderer/README.md](../../src/renderer/README.md) ("Cutout materials").
- Code: `shaders/cutout.glsl:15` (the threshold test), `src/renderer/passes/geometrypass.cpp:46` (opaque and cutout pipeline variants per cull
  mode), `src/renderer/gpuscene.h:87` (the material's opacity source), `src/renderer/gpuscene.h:92` (`opacityTexture`).
- Differs from plan: none found.
- Rejected alternatives: one pipeline with a per-material discard branch (no early depth for anything); linear texture formats now (deferred until
  normal or roughness maps arrive); no cutout shadows (wrong for foliage and fences).
