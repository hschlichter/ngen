# Renderer

`src/renderer/` turns a `RenderWorld` and a per-frame `RenderSnapshot` into command buffers. It runs on a dedicated render thread (`RenderThread`), builds a
frame graph each frame, and talks to the GPU only through the RHI (`src/rhi/README.md`). This file describes how a frame is built and the rules the code
relies on. Read it before adding a pass or changing how scene data reaches the GPU.

## Principles

- **GPU-driven is the only path.** Scene data lives in persistent GPU tables; visibility and draw lists are produced on the GPU; the mesh passes issue one
  indirect draw per pipeline bucket per view. There is no CPU-driven fallback to keep in sync.
- **The frame graph owns ordering and barriers.** A pass declares what it reads and writes; the graph orders passes, culls unused ones, allocates transient
  textures and inserts every texture and buffer barrier. Passes do not record barriers for graph resources.
- **Deterministic output.** The same scene and camera produce the same pixels, run to run. Screenshots are compared byte for byte, so anything that reorders
  draws (for example atomic appends) is a behaviour change, not an implementation detail.
- **What the CPU shows is read back.** Culling results, draw counts and the draw log come from the GPU after the frame slot's fence, so they are one
  frame-slot cycle old.

## Threads and handoff

The main thread builds a `RenderSnapshot` per frame (matrices, settings, the culling frustum, cascades, ImGui data, debug geometry) and hands it to the render
thread through a single slot with back-pressure. Scene uploads (`RenderUpload`: a `RenderWorld` plus mesh and material libraries) travel on a separate channel
and are applied at the start of a render frame by `Renderer::uploadRenderWorld`. Results for the editor come back the same way: the frame-graph debug snapshot,
the render-debug snapshot and the culling result (`RenderThread::latest*`), each latest-only.

## A frame

In execution order:

| Pass | Kind | Reads | Writes |
|---|---|---|---|
| `InstanceUpload` | copy, only when transforms changed | slot staging | instance buffer |
| `InstanceCull` | compute | cull params, instance buffer, mesh table | visibility bits, per-workgroup counters |
| `InstanceCullScan` | compute, one workgroup | per-workgroup counters | per-workgroup offsets, counts and statistics |
| `InstanceCullScatter` | compute | instance buffer, visibility, offsets | draw commands |
| `ShadowPass` | graphics, indirect | commands, counts, instance buffer | cascade atlas |
| `DepthPrepass` | graphics, indirect, optional | commands, counts | depth |
| `GeometryPass` | graphics, indirect | commands, counts, instance buffer, materials | G-buffer albedo and normals, depth |
| `CullReadback` | copy | counts, visibility, commands (render debugger only) | slot readback buffer |
| `LightingPass`, `AAPass`, blit, overlays, UI, present | | | |

`Renderer::render` builds the graph in that order; the graph derives the same order from the declared accesses.

## Scene data on the GPU

`GpuScene` owns the scene's GPU tables. It has two upload paths, chosen by the data:

- **Bulk** (large, rare, blocking through `GpuUploader`): the geometry pool, the mesh table and the material table. Rebuilt whenever
  `uploadRenderWorld` sees a change other than transforms (mesh, material, submesh range, instance count).
- **Delta** (small, per frame, through the graph): instance records. `uploadRenderWorld` tracks the lowest and highest changed instance; the next frame writes
  that span into the frame slot's staging buffer and `InstanceUpload` copies it into the persistent instance buffer.

Tables:

- **Geometry pool.** One interleaved `Vertex` buffer, one position-only buffer (12 bytes per vertex, for depth-only passes) and one 32-bit index buffer for
  every mesh. Indices stay mesh-local; a mesh is addressed by `firstIndex` and `vertexOffset`, and the vertex and position pools share `vertexOffset`.
- **Mesh table.** `GpuMeshEntry` per `MeshHandle::index`: `firstIndex`, `vertexOffset`, `indexCount`. A mesh outside the pool has `indexCount` 0.
- **Material table and texture slots.** Slot 0 is the fallback texture; each textured material gets the next slot, in instance order, up to
  `GpuScene::maxTextures` (1024). `GpuMaterial` holds the base colour texture slot. Untextured materials already carry their colour as a 1×1 texture.
- **Instance buffer.** One `GpuInstanceRecord` (112 bytes, std430) per `GpuInstance`: model matrix, material entry, mesh index, submesh `indexOffset` and
  `indexCount`, world bounds, and flags (`primFirst`, `doubleSided`, `boundsValid`). Vertex shaders read it at `gl_InstanceIndex`; every draw sets
  `firstInstance` to the instance index. The buffer grows by recreation; the old one goes through the deletion queue.

When a table or buffer is replaced, every descriptor set that names it is rewritten (`Renderer::rebuildGeometryDescriptorSets`, which also rebuilds the
culling sets and reports `GeometryDescriptorsRebuilt` with the reason).

## Culling and draw lists

`DrawLists` defines the command layout and owns the per-slot buffers; `InstanceCullPass` runs `shaders/instancecull.comp` in three modes.

- **Regions.** One region per (view, bucket). View 0 is the camera, views 1..4 the shadow cascades; bucket 0 is single-sided (back-face culling), bucket 1
  double-sided. Camera regions hold every visible instance with a mesh in the pool, drawing its submesh range. Cascade regions hold only `primFirst` instances
  (one per prim), drawing the whole mesh: shadows are material-agnostic.
- **Visibility.** Each instance's world bounds are tested against six normalized planes per view (`Frustum::fromViewProj`, computed on the CPU into the slot's
  parameter buffer) with the positive-corner test. Instances without valid bounds are always visible. With culling off every instance is visible. A frozen
  frustum is just a different camera matrix in the snapshot.
- **Ordered compaction (reduce-then-scan).** Cull counts, per workgroup of 256 instances, how many commands each region gets. Scan turns the per-workgroup
  counts into start offsets and totals. Scatter writes each command at its workgroup's offset plus its position in a shared-memory prefix sum. Commands stay in
  instance order, so draw order and depth-tie resolution match run to run. Atomic appends would be simpler and scale without the scan, but make the order
  depend on scheduling. The scan loops over workgroups on one thread per counter, so it stays cheap into the millions of instances.
- **Counts and statistics.** The totals buffer holds the draw count per region (read by `drawIndexedIndirectCount`), primitives per region, camera-culled
  instances, and culled and drawn `primFirst` instances per cascade.
- **Per-slot buffers.** Parameters, visibility, counters, offsets, commands and counts are per frame slot and imported into the graph each frame; the slot's
  fence guards them. The graph has no transient buffers yet: a pooled buffer reused in the next frame would need synchronisation against the previous frame's
  reads, and the graph starts every frame at `Undefined`.
- **Readback.** `CullReadback` copies the totals, the visibility bits and, while the render debugger is open, the commands into the slot's host-visible
  readback buffer. `DrawLists::parseReadback` reads it after the slot's fence. `RenderStats.culled`/`shadow_culled`, the Culling window, the AABB overlay, the
  per-pass `draws`/`primitives` (through `FrameGraphContext::addIndirectStats`) and the draw log all come from there.

The mesh passes bind the pipeline, descriptor set and pool once per bucket and issue one `drawIndexedIndirectCount` with the region's capacity as
`maxDrawCount`. An empty region still costs one call; only the GPU knows the count.

## Descriptor sets

| Set | Used by | Bindings |
|---|---|---|
| Geometry, one per frame slot | `GeometryPass`, `DepthPrepass` | 0 view UBO, 1 `sampler2D textures[1024]`, 2 instance buffer, 3 material table |
| Shadow, one | `ShadowPass` | 0 instance buffer; the cascade matrix is a push constant |
| Culling, one per frame slot | the three cull passes | params, instances, mesh table, visibility, counters, offsets, commands, counts |

The texture array is fixed-size and fully written (unused slots hold the fallback), and indexed with one value per draw: `gbuffer.vert` looks up the slot
through the instance's material and passes it flat to `gbuffer.frag`. That dynamically uniform indexing is a core Vulkan 1.0 feature and holds for
multi-draw indirect, where each draw is its own invocation group.

## Device requirements

Vulkan 1.2 with `shaderSampledImageArrayDynamicIndexing`, `multiDrawIndirect`, `drawIndirectFirstInstance` and `drawIndirectCount`. `RhiDeviceVulkan::init`
fails with a message if any is missing.

## Frame graph rules

- **Textures.** Created textures are transient: allocated from `ResourcePool` at first use and returned after last use. Imported textures (backbuffer, depth)
  start each frame undefined.
- **Buffers.** Buffers are imported only. `importBuffer(name, buffer, desc, initialAccess)` takes the access the previous frame left the buffer in, and
  `finalAccess(handle)` returns it after `execute`, so a persistent buffer (the instance buffer) synchronises across frames. The caller stores that state
  next to the buffer. Read-after-read of the same kind needs no barrier; anything involving a write does.
- **Stats.** A pass's `RhiCommandStats` include the barriers the graph issued for it. Indirect passes add their draws and primitives with `addIndirectStats`,
  since the RHI cannot see inside an indirect call.
- **Culling.** A pass without side effects whose outputs nobody reads is culled and reported with `PassCulled`.

## Shadows

Cascaded shadow maps in one atlas: one tile for a single cascade, a 2×2 grid otherwise (three cascades by default, `ShadowCascadeSettings`). The main thread
fits the cascades (`fitShadowCascades`: practical split scheme, bounding-sphere ortho frusta snapped to texels, far plane pulled in to the scene bounds). The
lighting pass samples the atlas with a hardware compare sampler (3×3 PCF by default).

## Textures

Material textures are RGBA8 sRGB with full mip chains built on the CPU (`mipchain.h`, 2×2 box filter) and uploaded with `GpuUploader`. The material sampler is
trilinear with anisotropy and editable live (`SamplerSettings`, `sampler` session verb); changing it rebuilds the geometry descriptor sets. The texture
inspector blits one level of one material texture into a preview for the Render Debug window.

## Observation

Render-category events (see `obs.md` for the conventions): `RenderStats` every 60 frames (`draws`, `indirect_draws`, `draw_commands`, `primitives`,
`instances`, `culled`, `shadow_culled`, `instance_buffer_bytes`, `instance_upload_bytes`, `geometry_pool_bytes`, `material_count`, ...), `GpuTime` per frame,
`CullReadback` per readback, `InstanceUpload` per upload frame (with the access carried in from the previous frame), and one-off events for table and buffer
creation (`GeometryPoolBuilt`, `MaterialTableBuilt`, `InstanceBufferCreated`, `DrawListsCreated`, `GeometryDescriptorsRebuilt`). `--dump-render-debug` writes
per-pass stats (`draws`, `indirectDraws`, `bufferBinds`, `descriptorBinds`, `barriers`, ...) and the draw log.

## Known gaps

- No per-draw GPU timing. A timing zone cannot open inside a multi-draw call; per-pass GPU times remain.
- The draw log and per-pass draw counts are read back, so they lag, and the log exists only while the render debugger is open.
- No transient buffers in the frame graph (see "Culling and draw lists").
- The texture array is fixed at 1024 slots; materials past it use the fallback texture and a warning is printed.
- Culling is per instance against frusta only: no occlusion culling, no clusters or meshlets. Building-sized meshes cull poorly.
- Everything runs on one graphics queue; the cull passes are separate graph passes so a multi-queue scheduler could move them.
