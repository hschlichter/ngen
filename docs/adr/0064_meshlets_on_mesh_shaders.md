# 64. Meshlets on `VK_EXT_mesh_shader` with occlusion culling

Date: 2026-10-15

## Status

Accepted, not implemented

Decided in the roadmap's vertical slice (step 2); no plan document or code yet.

## Context

GPU culling is per instance against frusta only ([ADR 29](0029_gpu_culling_ordered_compaction.md)). Building-sized meshes cull poorly: the airfield
at its finest level drew 106 million of 136 million triangles. [plan_gpu_driven.md](../plan_gpu_driven.md) deferred meshlets and depth-pyramid
occlusion until a packing step could produce meshlets, and that step now exists ([ADR 42](0042_separate_asset_server.md)). The mesh format and the
GPU scene change for this, and everything after it builds on them, so it comes before the rest of the slice.

## Decision

- The mesh packer splits meshes into meshlets (meshoptimizer, vendored), each with a bounding sphere and a normal cone. The mesh pack and the GPU
  scene gain a meshlet table.
- Mesh shaders draw them, culling per meshlet (frustum, back-facing by cone) after the per-instance stage.
- Two-phase occlusion culling against a depth pyramid (HZB) culls instances and meshlets hidden behind others; it is part of this step, because
  without it the savings in dense scenes don't show.
- `VK_EXT_mesh_shader` is required, approved beyond the Vulkan 1.3 baseline ([ADR 25](0025_vulkan_13_baseline.md)). A device without it fails at
  start-up with a clear message. Whether CI's lavapipe has it is checked first.
- A meshlet debug view, and meshlet and occlusion counts in the `culling` record.

Deferred: a path without mesh shaders (a possible topic later), and cluster LOD in the Nanite style (UsdLod stays the only LOD,
[ADR 50](0050_usdlod_only_lod_convention.md)).

## Consequences

- Easier: the measured triangle bottleneck on large meshes is addressed at its cause.
- Easier: occlusion culling makes dense scenes cheaper, not only large meshes.
- Harder: device support narrows to drivers with mesh shaders.
- Harder: CI on lavapipe may need another answer.
- Harder: the mesh pack format, the GPU scene and the culling passes all change at once.

## Verification

- Sources: [roadmap.md](../roadmap.md) (vertical slice, step 2, decided 2026-10-15), [plan_gpu_driven.md](../plan_gpu_driven.md) ("Out",
  deferred).
- Code: no `mesh_shader` in `src/rhi`; `src/renderer/README.md:450` lists "no occlusion culling, no clusters or meshlets" as a known gap.
