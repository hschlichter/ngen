# 50. UsdLod is the only LOD convention

Date: 2026-10-11

## Status

Accepted

Amended by [ADR 68](0068_render_lod_on_gpu.md): not every level is a payload — a root whose levels aren't payloads is render LOD, picked on the GPU.

## Context

OpenUSD 26.08 ships the UsdLod schema: `LODRootAPI` on a root whose children are its levels, heuristics by domain (distance, screen size), and
overrides. Caldera, the large test scene of the time, expresses LOD differently — as `districtLod` and `modelLod` variant sets — and its levels sit
inside instanceable assets that no outside opinion can change. Supporting several LOD conventions in the engine would spread knowledge of particular
datasets through it.

## Decision

- The engine reads only UsdLod. A LOD root's defined children are its levels, finest first; its heuristics are the selection rule.
- Data authored another way is converted before packing, outside the engine. Caldera is rewritten by a Python converter on a branch of its fork:
  every level's arc becomes a payload, and screen-size thresholds are generated.
- As first decided, every level is a payload, so every LOD root streams.
- Nanite-style cluster LOD is deferred until games or other use cases show what LOD they need.

## Consequences

- Easier: one code path that knows nothing about any dataset.
- Easier: picks can be checked against OpenUSD's own (`ngen-usdcheck --lod-picks`).
- Harder: content in another convention needs a conversion step; Caldera's thresholds are the converter's guesses.
- Harder: there is no automatic LOD generation.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_streaming_lod.md](../plan_streaming_lod.md) (decision 2), [plan_caldera_usdlod.md](../plan_caldera_usdlod.md),
  [roadmap.md](../roadmap.md) (cluster LOD), [src/world/README.md](../../src/world/README.md) ("Loading: payloads and LOD levels").
- Code: `src/world/world.h:196` (`LodRoot`), `src/apps/usdcheck.cpp:5` (`--lod-picks`), `src/renderer/README.md:450` ("no clusters or meshlets").
- Differs from plan: "every level is a payload" holds only for roots that stream; roots with inline levels are render LOD
  ([ADR 68](0068_render_lod_on_gpu.md)).
- Rejected alternatives: an override layer over Caldera (it can't reach `modelLod` inside instanceable assets); cluster LOD now (premature).
