# 64. Meshlets on `VK_EXT_mesh_shader` with occlusion culling

Date: 2026-10-15

## Status

Accepted

Decided in the roadmap's vertical slice (step 2). Implemented by [plan_meshlets.md](../plan_meshlets.md),
[plan_meshlet_culling_visibility.md](../plan_meshlet_culling_visibility.md), [plan_occlusion_culling.md](../plan_occlusion_culling.md) and
[plan_meshlet_performance.md](../plan_meshlet_performance.md), all landed. Where the code goes beyond this decision is under Verification.

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
  deferred); the four plans under Status; [meshlets_learnings.md](../meshlets_learnings.md) and the review,
  [meshlets_review.md](../reviews/meshlets_review.md).
- Code:
  - Packing: `src/asset/pack/meshletbuild.cpp` (meshoptimizer, `external/meshoptimizer`), called from `src/asset/pack/usd/usdpacker.cpp`;
    `src/asset/README.md`.
  - Device: `src/rhi/vulkan/rhidevicevulkan.cpp` requires `taskShader` and `meshShader` and fails at start-up with a message; lavapipe has
    them, so CI runs `ngen-example-meshshader` (`src/rhi/README.md`, "Required features", "Examples").
  - Drawing and meshlet culling: `shaders/meshlet.task`, `meshlet.glsl`, the `*.mesh` shaders; every mesh pass draws through them, one
    `drawMeshTasksIndirect` per region (`src/renderer/README.md`, "Meshlets").
  - Occlusion: `src/renderer/depthpyramid.{h,cpp}`, `shaders/hzbbuild.comp`, `hzb.glsl`, `InstanceCullPass::addLatePasses`
    (`src/renderer/README.md`, "Occlusion culling").
  - Records and views: the `culling` record's meshlet and `occlusion` counts, the `meshlet` and `meshletcull` debug views, the Culling window.
- Differs from the decision:
  - **More of the device is required.** Besides `VK_EXT_mesh_shader`, the culling scan's prefix sums need subgroup basic and arithmetic
    operations in compute shaders, which Vulkan 1.3 doesn't guarantee (a device property, checked in `rhidevicevulkan.cpp`, not a feature);
    and every shader is compiled for Vulkan 1.3 (`src/apps/packershader.cpp`), so `maintenance4` and `shaderDemoteToHelperInvocation`,
    core in 1.3, are enabled. RADV, ANV, NVK and lavapipe have all of it.
  - **No path without mesh shaders.** The vertex path was removed rather than kept beside the mesh path; a fallback stays deferred as above.
  - **Occlusion's first phase draws in the geometry pass by default**, not in a depth prepass: the prepass drew every first-phase triangle
    twice and cost more than occlusion saved. With the prepass on, both phases run there. Either way the first phase's meshlets aren't tested
    against a pyramid when they are first drawn (no per-meshlet history).
  - **Not built**: the pyramid view ([plan_occlusion_culling.md](../plan_occlusion_culling.md), "As built"); the Engine Map nodes the
    roadmap names, as the Engine Map no longer exists ([ADR 62](0062_runtime_access_and_visibility.md)).
