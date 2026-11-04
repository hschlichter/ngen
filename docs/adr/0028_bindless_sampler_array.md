# 28. Bindless materials as a fixed sampler array

Date: 2026-09-25

## Status

Accepted

Amended by [ADR 54](0054_async_scene_and_incremental_gpu_scene.md): texture slots are assigned incrementally as textures arrive, with a white
"uploading" slot.

Amended by [ADR 56](0056_cutout_buckets.md): `GpuMaterial` also carries an opacity slot, channel and threshold.

## Context

Descriptor sets were allocated per instance per frame slot to bind each material's texture. Multi-draw indirect (ADR 26) draws many instances in one
call, so the material has to be looked up per draw in the shader, without rebinding.

Full descriptor indexing (runtime-sized arrays, partially bound, update-after-bind, `nonuniformEXT`) would do it, but needs features beyond the
baseline the project had just fixed (ADR 25).

## Decision

- One binding, `sampler2D textures[1024]`, with every slot written; unused slots hold the fallback texture.
- The slot is found through instance → material in the vertex shader and passed flat to the fragment shader, so the index is dynamically uniform per
  draw. That needs only the core `shaderSampledImageArrayDynamicIndexing` feature.
- Combined image samplers.
- One geometry descriptor set per frame slot holds the UBO, the textures, and the instance, material and LOD tables.

Deferred: full descriptor indexing (until texture streaming or a scene beyond 1,024 textures), separate images plus one sampler (until a second backend
revisits the descriptor model), and splitting the set into per-frame and per-scene halves.

## Consequences

- Easier: no extensions, and one descriptor bind per bucket.
- Harder: a hard ceiling of 1,024 textures; past it, textures fall back with a warning.
- Harder: the set is rewritten whole when slots change, and a sampler change rebuilds it.

## Verification

- Sources: [plan_bindless_materials.md](../plan_bindless_materials.md), [src/rhi/README.md](../../src/rhi/README.md) known gaps,
  [src/renderer/README.md](../../src/renderer/README.md) "Descriptor sets".
- Code: `shaders/gbuffer.frag:4` (`textures[1024]`), `:19`; `src/renderer/gpuscene.h:90` (`GpuMaterial`), `:147` (`maxTextures = 1024`).
