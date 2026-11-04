# 66. Variant sets and PointInstancer in the World

Date: 2026-10-16

## Status

Accepted

## Context

The scene pack work ([ADR 52](0052_view_renders_layer_packs.md), [ADR 53](0053_worldcomposer_composes_as_pcp.md)) listed variant sets as out of
scope, and the packer warned and dropped `PointInstancer`. The streaming test world ([ADR 65](0065_test_world_over_caldera.md)) uses both: every
placed asset selects a `shape` variant (around 11,200 trees per tile), and scatter is authored with `PointInstancer`. Streaming v2 step 4 could not
be tested without them.

## Decision

- The USD packer packs variant prim specs at their variant paths, as Sdf writes them.
- `WorldComposer` adds variant nodes as Pcp does: the strongest selection anywhere in the prim index wins, nested sets work, and there are no
  fallbacks. A LOD root's levels are its defined children, variants included.
- A `PointInstancer`'s arrays (prototypes, positions, orientations, scales, invisible ids) are kept as a World component.
- Each visible point draws its prototype at the instancer's world transform times the point's, by replaying the prototype's draws per point.
- Parity with OpenUSD is checked with `ngen-usdcheck --instancers`.

The plan's decisions 1 to 6 were each taken as recommended; the drawing approach is option A, replay per point.

## Consequences

- Easier: best-practice USD works without rewriting it.
- Easier: instances with different variant selections get different prototypes, as in USD.
- Harder: variant fallbacks are not supported; a set with no selection composes nothing.
- Harder: replay per point costs one draw per point and prototype mesh, with no GPU-side instancing of points.

## Verification

- Sources: [plan_variants_pointinstancer.md](../plan_variants_pointinstancer.md), [plan_scene_pack.md](../plan_scene_pack.md) ("Out"),
  [test_streaming_world.md](../test_streaming_world.md).
- Code: `src/world/world.h:212` (`PointInstancerData`), `:247` (`pointInstancers`); `src/world/worldcomposer.cpp:43-68` (variant selections in node
  paths); `src/view/README.md:157` (each visible point draws its prototype); `src/view/README.md:54` (the `instancers` and `variants` records).
