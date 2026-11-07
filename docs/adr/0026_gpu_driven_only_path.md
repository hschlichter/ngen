# 26. GPU-driven is the only path

Date: 2026-09-25

## Status

Accepted

Amended by [ADR 56](0056_cutout_buckets.md): buckets became cull mode × cutout, four per view instead of two.

## Context

Drawing was CPU-driven: one `drawIndexed` per instance and submesh, a descriptor set per instance, a vertex and index buffer per mesh. Both CPU cost
and draw-call cost grew with the number of instances, which the planned scenes (Caldera, streaming worlds) would multiply by orders of magnitude.

The umbrella plan says outright that its five stages buy scale and a base for meshlets, not Sponza's GPU time.

## Decision

The GPU owns the scene, built in five stages (frame-graph buffers, geometry pool, bindless materials, indirect draws, GPU culling):

- Persistent GPU tables — instances, one geometry pool, the mesh table, the material table. The CPU sends changes, not whole tables.
- Every draw command sets `firstInstance` to its instance slot, and shaders read `instances[gl_InstanceIndex]`; no `shaderDrawParameters` or
  `gl_DrawID` needed.
- Pipeline buckets (sidedness, later cutout) each own a region of the command buffer. A pass issues one `drawIndexedIndirectCount` per bucket per view.
- Indices are 32-bit only.
- No CPU fallback and no toggle. Per-draw timing (`Time draws`, `LargeDraw` zones) was removed; GPU zones are per region.

Rejected: keeping a CPU path (doubles every pass), 16-bit indices per mesh (doubles the buckets), several geometry pools (more binds per bucket).

## Consequences

- Easier: draw count and CPU cost no longer scale with instance count.
- Easier: meshlets can extend the culling pass later (ADR 64).
- Harder: per-draw GPU timing is impossible; the finest GPU time is a region.
- Harder: draw counts and the draw log come back from the GPU a frame slot late.
- Harder: an empty region still costs one call, and the instance record grew from 64 to 128 bytes as features were added.

## Verification

- Sources: [plan_gpu_driven.md](../plan_gpu_driven.md), [plan_frame_graph_buffers.md](../plan_frame_graph_buffers.md),
  [plan_geometry_pool.md](../plan_geometry_pool.md), [plan_indirect_draws.md](../plan_indirect_draws.md), [plan_gpu_culling.md](../plan_gpu_culling.md),
  [src/renderer/README.md](../../src/renderer/README.md) "Principles".
- Code: `drawIndexedIndirectCount` at `src/renderer/passes/geometrypass.cpp:193`, `depthprepass.cpp:106`, `shadowpass.cpp:171`, `debugviewpass.cpp:241`;
  `shaders/gbuffer.vert:10` and `:69` (`instances.data[gl_InstanceIndex]`); `src/renderer/gpuscene.h:58` (`GpuInstanceRecord`), `:73` (128 bytes).
- Since [ADR 64](0064_meshlets_on_mesh_shaders.md) (2026-11-07): the decision holds, the mechanism changed. The mesh passes draw meshlets
  with task and mesh shaders, one `drawMeshTasksIndirect` per region (`DrawLists::drawRegion`, `src/renderer/drawlists.cpp:454`, called from
  `geometrypass.cpp`, `depthprepass.cpp`, `shadowpass.cpp`, `debugviewpass.cpp`); there is no index buffer and no `gbuffer.vert`. The task and
  mesh shaders read the instance record at the slot a meshlet work item names, and the culling passes read a 48-byte culling record beside it.
