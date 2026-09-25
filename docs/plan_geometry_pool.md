# Global geometry pool

**Status. In progress.** Implemented and verified headless; the prim-creation check is pending a look in the editor.

Stage 2 of [plan_gpu_driven.md](plan_gpu_driven.md).

## Current state

Every mesh owns three GPU buffers (`CachedMesh` in `src/renderer/passes/geometrypass.h`): the interleaved `Vertex` stream, the position-only stream
(`docs/plan_position_stream.md`), and a 32-bit index buffer. Draws rebind them:

- The geometry pass and the depth prepass call `bindVertexBuffer` and `bindIndexBuffer` for every instance, then
  `drawIndexed(inst.indexCount, 1, inst.indexOffset, 0, m)`.
- The shadow pass does the same per mesh per cascade, drawing the whole mesh.

`Renderer::uploadRenderWorld` is all-or-nothing for geometry. When the instance list changes anything but transforms (`geometryChanged`), it defers
deletion of every mesh buffer and texture, then re-uploads all of them through `GpuUploader`. `GpuUploader` is a one-shot command buffer: one staging
buffer per upload, one submit, and a blocking fence wait in `end()`. Transform-only changes go through the stage 1 instance buffer and don't touch
geometry.

Indirect draws (stage 4) can't rebind vertex or index buffers per draw. Every draw in a multi-draw call must come from one bound vertex/index pair,
addressed by `firstIndex` and `vertexOffset`.

## Scope

**In**

- Three pool buffers: `Vertex` stream, position stream, 32-bit indices. They are built from all meshes whenever geometry changes, and replace the
  per-mesh buffers.
- A CPU mesh table: per mesh, `firstIndex`, `vertexOffset`, `indexCount`, `vertexCount` and byte sizes. The Render Debug mesh stats read it.
- The geometry, prepass and shadow passes bind the pool once per pass (per pipeline bucket, where binds reset). Draws use the table offsets.

**Out**

- A GPU copy of the mesh table (bounds, offsets). Nothing reads it on the GPU until stage 4 or 5; it arrives with its first reader.
- Incremental add and remove of single meshes (free list, compaction). See decision 2.
- 16-bit indices. See decision 4.
- Vertex pulling (reading vertices from a storage buffer instead of vertex input). It isn't needed for indirect draws.
- Textures. They keep their full re-upload on geometry change, and bindless (stage 3) revisits them.

## Decisions

All four recommendations were confirmed and are locked.

1. **Upload path: one shared "GPU scene upload" or one path per table?** This is the open question from the umbrella plan.
   - (a) Ad hoc: the pool code lives in `Renderer` beside the instance-buffer code, each with its own upload logic.
   - (b) A `GpuScene` class (`src/renderer/gpuscene.h/.cpp`) owns the scene's GPU tables: the instance buffer (moved from `Renderer`), the geometry
     pool, and later the material table (stage 3) and the mesh table's GPU copy. It exposes two upload paths, split by the nature of the data:
     - **Bulk:** large, rare, blocking, through `GpuUploader`. The geometry pool on scene load or geometry change.
     - **Delta:** small, every frame, through per-slot staging and the `InstanceUpload`-style graph pass. Transforms now, material edits in stage 3.

   I lean (b). Geometry is hundreds of MB on Sponza and can't go through a per-frame staging slot, while transforms and material edits must not
   stall. So "one path" would be the wrong abstraction and "one owner, two paths" is the honest one. It also moves scene GPU state out of
   `renderer.cpp` (47 KB) before stages 3 to 5 add to it. Cost: moving the stage 1 code, a mechanical move checked by the same byte-identical
   screenshots.
2. **Mesh removal: rebuild the pool on every geometry change, or a free list?** I lean rebuild. It is exactly today's behaviour: all meshes
   re-upload on any geometry change. A free list or compaction needs a reason to exist, meaning incremental geometry changes. Trigger: prim creation
   or streaming makes the full rebuild visible in a profile (`docs/plan_incremental_gpu_upload.md`, asset streaming after packing).
3. **Capacity: sized exactly at each rebuild, or grown with headroom?** Since every change is a rebuild, I lean exact size, with no growth policy.
   Headroom comes with the free list in decision 2.
4. **Index width: 32-bit only, or 16-bit per mesh where it fits?** I lean 32-bit only. One index buffer means one index type per bind, and stage 4's
   multi-draw uses one index type per call. Two pools by width would double the buckets. Sponza's indices are already 32-bit, so memory is unchanged.

## Steps

1. **`GpuScene`** (`src/renderer/gpuscene.h/.cpp`). Move `instanceBuffer`, its staging, capacity, dirty span, `ensureInstanceCapacity`,
   `markInstancesDirty` and the per-frame upload (the `importBuffer` / `addInstanceUploadPass` block in `Renderer::render`) out of `Renderer`. The
   renderer keeps `gpuInstances` for now, and `GpuScene` reads spans of it.
   ```cpp
   struct GpuMeshRange {
       uint32_t firstIndex = 0;
       int32_t vertexOffset = 0;
       uint32_t indexCount = 0;
       uint32_t vertexCount = 0;
   };

   class GpuScene {
   public:
       auto init(RhiDevice* device, uint32_t frameSlots, DeletionQueue* deletionQueue) -> void;
       auto destroy() -> void;

       // Bulk: rebuilds the geometry pool from every mesh the instances reference (blocking upload).
       auto rebuildGeometry(std::span<const GpuInstance> instances, const MeshLibrary& meshLib, GpuUploader& uploader, uint64_t frame) -> void;
       auto meshRange(uint32_t meshIndex) const -> const GpuMeshRange*;

       // Delta: instance transforms.
       auto updateInstances(std::span<const GpuInstance> instances, uint32_t dirtyFirst, uint32_t dirtyEnd, uint64_t frame) -> void;
       auto addUploadPasses(FrameGraph& fg, uint32_t frameSlot) -> FgBufferHandle; // returns the instance handle
       auto afterExecute(const FrameGraph& fg) -> void;                             // stores the carried access

       auto vertexBuffer() const -> RhiBuffer*;
       auto positionBuffer() const -> RhiBuffer*;
       auto indexBuffer() const -> RhiBuffer*;
       auto instanceBuffer() const -> RhiBuffer*;
   };
   ```
2. **Pool build** (`GpuScene::rebuildGeometry`). Walk the unique meshes in instance order, append their vertices, positions and indices into three
   CPU vectors, and record a `GpuMeshRange` per mesh. Upload each vector with `uploader.uploadBuffer` in one `begin`/`end`, then defer deletion of the
   old pool buffers. Indices stay mesh-local, and `vertexOffset` rebases them, so index data is copied unchanged.
3. **Remove `CachedMesh`'s buffers.** `meshCache` becomes the mesh table (`GpuMeshRange` plus byte sizes for the debug stats). `uploadRenderWorld`
   calls `rebuildGeometry` where it re-uploads meshes today. Textures keep their loop.
4. **Passes.**
   - Geometry pass and prepass: `bindVertexBuffer(pool)` and `bindIndexBuffer(pool, Uint32)` once after each pipeline bind. Each draw becomes
     `drawIndexed(inst.indexCount, 1, range.firstIndex + inst.indexOffset, range.vertexOffset, m)`.
   - Shadow pass: bind the position pool and the index pool once per bucket per cascade, then
     `drawIndexed(range.indexCount, 1, range.firstIndex, range.vertexOffset, m)`.
   - The pass signatures take the pool buffers (or `const GpuScene&`) instead of the per-mesh `CachedMesh` buffers.
5. **Observation.** A `GeometryPoolBuilt` event per rebuild with `meshes`, `vertex_bytes`, `position_bytes`, `index_bytes` and `ms`. `RenderStats`
   gains `geometry_pool_bytes`.

## Verification

Baseline: the stage 1 screenshots (byte-identical to the pre-stage-1 ones) and the stage 1 `--dump-render-debug` output, same scenes and flags.

- Screenshots byte-identical, prepass off and on. **Verified**: all six pairs identical (three_cubes, Sponza courtyard, Sponza framed on the scene,
  each with and without prepass).
- `--fail-on-validation` exits 0. **Verified** on all runs. The RHI examples pass `--check --validation`, since `RhiCommandStats` gained a field.
- Buffer binds, via the new `bufferBinds` counter in `RhiCommandStats` and the dump. **Verified** on Sponza at the courtyard camera:
  - `ShadowPass` has 6 binds for 204 draws (one vertex and index pair per cascade, three cascades). It was 408, one pair per draw.
  - `GeometryPass` has 2 binds for 65 draws. It was 130.
  - three_cubes with prepass: `DepthPrepass` 2 binds for 4 draws.
  - `draws` and `primitives` are unchanged in every pass.
- Pool size. **Verified**: `GeometryPoolBuilt` reports 115 meshes, and 494.6 MB vertex, 134.9 MB position and 45.0 MB index data. The total,
  674,463,960 bytes, equals the sum of the old per-mesh `vertexBytes + indexBytes` in the stage 1 dump, and `RenderStats.geometry_pool_bytes`
  reports the same number. The build takes 938 ms on Sponza, CPU concatenation plus the blocking upload, and it runs once on load.
- The instance path moved into `GpuScene` unchanged. **Verified**: the `translate` script from stage 1 gives the same `InstanceUpload` events
  (`carried_access` `StorageRead` on the move), and the after-move screenshot is byte-identical to the stage 1 one.
- Prim creation in the editor (`docs/plan_prim_creation.md`) triggers one `GeometryPoolBuilt`, and the new prim renders. **Pending**: UI only.
- GPU time on Sponza within noise. **Verified**: `ShadowPass` 3.65 to 3.65 ms, `GeometryPass` 1.21 to 1.27 ms.

## Deferred / follow-ups

- Free list and incremental mesh add and remove, with headroom. Trigger in decision 2.
- A GPU copy of the mesh table, with bounds: with stage 4 or 5, whichever reads it first.
- 16-bit indices: only if memory becomes the constraint, as a second bucket dimension.
