# Meshlets: meshes split at pack time, drawn by mesh shaders, culled per meshlet

**Status. Landed.** Decisions 1–9 locked 2026-10-22, by the agent on the user's instruction ("make all the decisions"); the reasons are kept
so each can be revisited. The first half of the roadmap's vertical slice step 2; occlusion culling is the second half,
[plan_occlusion_culling.md](plan_occlusion_culling.md), which builds on this one.

## Current state

- **Packing.** `ngen-packer-usd` writes one mesh asset per prim (`file.usd#/prim`, `src/asset/pack/scenepacktypes.h` `MeshPack`): 44-byte
  `PackVertex`es, 32-bit indices, submeshes as index ranges (one per material-bound `GeomSubset`, contiguous). Polygon meshes are *not welded*:
  `polygonMesh` emits three new vertices per triangle (`usdpacker.cpp` "for (int v = 0; v < 3; v++)"), so a mesh has 3 × its triangles in
  vertices. Format version 4, `usd` rule version 8. meshoptimizer is not vendored.
- **GPU scene.** One interleaved vertex pool, a position pool, a 32-bit index pool; `GpuMeshEntry {firstIndex, vertexOffset, indexCount}`; a
  draw slot per submesh use (`GpuInstanceRecord`, 128 bytes, `indexOffset`/`indexCount` its range).
- **Culling.** `instancecull.comp` (cull, scan, scatter) writes one `drawIndexedIndirect` command per visible instance per region (view ×
  bucket); every mesh pass draws a region with one `drawIndexedIndirectCount`; vertex shaders take attributes and the instance at
  `gl_InstanceIndex`. Render LOD (`plan_render_lod.md`) drops the slots of levels a group doesn't draw in the same cull pass.
- **RHI.** Vertex, fragment, compute and geometry stages only; no mesh shading (`src/rhi/README.md`).
- Building-sized meshes cull poorly (`src/renderer/README.md` "Known gaps").

## Scope

In:
- meshoptimizer vendored; the packer welds vertices and splits each submesh into meshlets with bounds (sphere, normal cone).
- Mesh pack format 5 with meshlet sections; the mesh library, `MeshDesc` and the GPU scene carry meshlets.
- The RHI: task and mesh shader stages, `drawMeshTasksIndirectCount`, `VK_EXT_mesh_shader` required; an RHI example.
- Every mesh pass draws through task and mesh shaders: geometry, depth prepass, shadow, debug views. The indexed path, the GPU index pool,
  the mesh-pass vertex shaders and the debug views' geometry shader go.
- Per-meshlet frustum culling in every view, backface culling by normal cone in the camera's single-sided buckets.
- A meshlet debug view, meshlet counts in the `culling` record and the Culling window, meshlet memory in `memory`, nodes in the Engine Map.

Out:
- Occlusion culling: [plan_occlusion_culling.md](plan_occlusion_culling.md).
- Cluster LOD (simplified meshlet groups): roadmap, Rendering, deferred on purpose.
- Compressed vertex formats, 16-bit meshlet indices into an 8-bit-per-index stream: Deferred.
- A path without mesh shaders (roadmap decision 2026-10-15).

## Decisions

### 1. Weld at pack time

Meshlets bound unique vertices (64 here). On unwelded meshes a meshlet holds 21 triangles instead of about 124, six times the meshlets and
three times the vertex memory. **Locked: weld every mesh in the packer** (`meshopt_generateVertexRemap` over the whole `PackVertex`, so only
bitwise-identical vertices merge: a hard edge keeps its split normals). Index order is kept, so submesh ranges don't move.

### 2. Meshlet size and build

**Locked: at most 64 vertices and 124 triangles**, built per submesh (a meshlet never mixes materials) with `meshopt_buildMeshlets`
(cone weight 0.25) after `meshopt_optimizeVertexCache` on the submesh's range, then `meshopt_optimizeMeshlet`, bounds from
`meshopt_computeMeshletBounds`. 64/124 is meshoptimizer's recommendation for both vendors and fits a 64-thread mesh workgroup (one vertex and
two triangles per thread); RADV reports `maxMeshOutputVertices` 256, `maxPreferredMeshWorkGroupInvocations` 128.

### 3. The pack format

**Locked**: three sections beside the existing four: `MeshMeshlets` (per meshlet: vertex offset, triangle offset, vertex and triangle count,
sphere centre and radius, cone axis and cutoff as meshoptimizer's 8-bit form), `MeshMeshletVertices` (32-bit, mesh-local vertex indices),
`MeshMeshletTriangles` (one 32-bit word per triangle, three 8-bit meshlet-local indices: one read per triangle in the shader, 33% larger than
packed bytes). `PackSubmesh` gains its meshlet range. The index list stays in the pack: it is the source the meshlets are built from and the
CPU's (bounds, tools); only the GPU stops using it. Format version 5, `usd` rule version 9.

### 4. How meshlets are dispatched

- **A. One task draw per visible instance**: the scatter writes a `{groupCountX, 1, 1, instance}` command (stride 16) per visible instance
  per region, as today's indexed command; `drawMeshTasksIndirectCount` per region; the task shader finds its instance at `gl_DrawID`.
- **B. One draw per region over work items** (an instance's meshlets in chunks of 32, each a work item): far fewer draws, but one draw then
  spans many instances, so the material's texture index is no longer uniform per draw and needs non-uniform descriptor indexing (an optional
  Vulkan 1.2 feature, not approved).

**Locked: A** at first: the cull, scan and scatter stay as they are but for the command they write, and each draw is still one instance, so
the renderer README's rule "indexed with one value per draw" holds. Trigger for B: the per-draw cost shows in the GPU zones at the test
world's densest viewpoint.

**Revised to B (2026-10-22), on that trigger.** Measured in release at the test world's dense_forest: the GPU frame went from 30.0 ms
(indexed) to 92.6 ms, nearly all of it in the double-sided cutout regions: 93,467 camera draws and 182,265 in cascade 2, about 23 triangles
each (grass and foliage), at about 0.25 µs a draw against the indexed path's 0.05. B as built: the scatter lays every drawn instance's
meshlets out as work items (`{instance, meshlet}`, in instance order) in one buffer per frame slot, the scan writes each region's draw
(`RegionWork`, 32 meshlets a task workgroup), every pass issues one `drawMeshTasksIndirect` per region, and a task workgroup takes any 32
work items, whatever their instance. The texture index then differs within a draw: `shaderSampledImageArrayNonUniformIndexing` (a Vulkan
1.2 feature; RADV and lavapipe have it) is required and the fragment shaders index with `nonuniformEXT`. Per-instance draw records stay, for
the draw log and the tools. The baselines are byte-identical to A's.

### 5. The task and mesh shaders

**Locked**: a task workgroup of 32 threads, one meshlet each: frustum test of the meshlet's sphere (the instance's model applied, the
radius scaled by its largest axis) against the region's view planes; the normal-cone test (decision 6); the survivors compacted in order
(a prefix in shared memory, not atomics or subgroup ballots: lavapipe's subgroups are narrower than 32, and order must not depend on
scheduling) into the payload, `EmitMeshTasksEXT(count)`. A mesh workgroup of 64 threads draws one meshlet: each thread reads at most one
vertex (pulled from the pool by index) and writes at most two triangles. Shared code in `shaders/meshlet.glsl`.

### 6. Which views cull how

**Locked**: the camera tests sphere and cone; the cone only in single-sided buckets (a double-sided surface shows its back) and only for an
instance whose model has uniform scale (a non-uniform scale bends the cone; such instances skip the test, which is conservative). Cascades
test the sphere against their planes, no cone (the shadow pass draws both faces' depth today, cull mode none for double-sided).

### 7. Every mesh pass moves; the indexed path goes

**Locked.** GPU-driven is the only path (`src/renderer/README.md` "Principles"): keeping the indexed draws beside meshlets is the fallback the
roadmap ruled out. The debug views' geometry shader (triangle size, edge distances, primitive index) becomes the debug views' mesh shader,
which sees whole triangles anyway; for the wireframe's edge distances it emits unshared vertices, so it splits a meshlet into two halves
(at most 62 triangles, 186 vertices, under the 256 limit). The geometry shader stays in the RHI (an example uses it) but the renderer no
longer needs it.

### 8. Statistics

**Locked**: the camera's geometry pass and the shadow pass count, in their task shaders, meshlets tested, culled by frustum, culled by cone,
drawn, and triangles drawn (atomic sums: their total doesn't depend on order), per view, into a per-slot buffer read back with the culling
readback. The prepass draws the same meshlets as the geometry pass and doesn't count. The `primitives` the passes report stay the culled
instances' triangles (an upper bound); the meshlet counts say what was drawn.

### 9. Determinism

Mesh shading orders primitives by draw, then task workgroup, then mesh workgroup, then primitive index (Vulkan spec, "Primitive Order").
The commands are in instance order (unchanged), the task shader compacts in meshlet order, so a frame's draw order is fixed. **Locked**:
screenshots stay byte-comparable run to run. Against the indexed path they can differ where depth ties resolve differently (meshlet order
isn't index order); the baselines are compared and every difference is explained or the baselines are re-taken, in the step that changes
them.

## Steps

1. **meshoptimizer and the packer.**
   - `external/meshoptimizer` submodule (latest release tag), `cxx::static_library("meshoptimizer")` in `build.cpp`, linked by
     `ngen-packer-usd` only.
   - `src/asset/pack/meshletbuild.{h,cpp}` (C++23, outside the C++20 USD file): `auto weldMesh(MeshPack& mesh) -> void;` and
     `auto buildMeshlets(MeshPack& mesh) -> void;`, called from `packMesh` after the mesh is built.
   - `scenepacktypes.h`:
     ```cpp
     struct PackMeshlet {
         uint32_t vertexOffset = 0;   // into MeshPack::meshletVertices
         uint32_t triangleOffset = 0; // into MeshPack::meshletTriangles
         uint32_t vertexCount = 0;
         uint32_t triangleCount = 0;
         std::array<float, 3> center = {0, 0, 0}; // bounding sphere, mesh space
         float radius = 0.0f;
         std::array<int8_t, 4> cone = {0, 0, 0, 0}; // axis xyz and cutoff, meshopt_Bounds' 8-bit form
     };
     ```
     `MeshPack` gains `meshlets`, `meshletVertices` (`uint32_t`), `meshletTriangles` (`uint32_t`, `i0 | i1 << 8 | i2 << 16`);
     `PackSubmesh` gains `meshletOffset`, `meshletCount`. `scenePackFormatVersion` 5; sections 15–17 in `scenepack.cpp`; `pack.cpp` `usd`
     version 9.
   - `ngen-packdump`: vertices, triangles, meshlets, average triangles per meshlet per mesh.
   - `src/asset/README.md`: the mesh pack's sections.
2. **The RHI.**
   - `RhiShaderStage::Task`, `Mesh`; `RhiGraphicsPipelineDesc::taskShader`, `meshShader` (a mesh pipeline has no vertex shader and no vertex
     input); `RhiCommandBuffer::drawMeshTasksIndirectCount(commands, offset, stride, count, countOffset, maxDrawCount)` and
     `RhiDrawMeshTasksIndirectCommand {x, y, z}`; `RhiDeviceLimits` mesh output limits.
   - Vulkan: `VK_EXT_mesh_shader` with `taskShader` and `meshShader` required at `init` (a clear message without them); stage and pipeline
     mapping; barrier states that cover the task and mesh stages (`StorageRead`, `IndirectRead`, `UniformRead`, sampled).
   - `ngen-example-meshshader`: a task shader culls half of a grid of quads by a push-constant plane, the mesh shader draws the rest with
     per-primitive colours; `--check` reads the pixels; screenshot hash added to `screenshots.sha256` (lavapipe).
   - The shader packer: `.task` and `.mesh` in `pack.cpp`, glslc with `--target-env=vulkan1.3` for them (SPIR-V 1.4 is needed by
     `GL_EXT_mesh_shader`); rule version 2.
   - `src/rhi/README.md`: stages, the draw, the required feature, the example.
3. **Meshlets in the GPU scene.**
   - `MeshDesc` gains `meshlets` (`MeshletDesc`, as `PackMeshlet`), `meshletVertices`, `meshletTriangles`; `SubMesh` gains its meshlet range
     and triangle count; `ViewScene::mesh` fills them.
   - `GpuScene`: pools `meshlets` (`GpuMeshlet`, 32 bytes: offsets, counts packed `vertexCount | triangleCount << 8`, cone packed, sphere),
     `meshletVertices`, `meshletTriangles`, each a `GrowableBuffer` with its own `PoolAllocator`; the index pool goes; the vertex and position
     pools gain `Storage` usage. `GpuMeshEntry {vertexOffset, meshletOffset, meshletCount, triangleCount}` (meshlet vertex indices are
     mesh-local; the shader adds `vertexOffset`). `GpuInstanceRecord`: `indexOffset`/`indexCount` become the submesh's `meshletOffset`
     (mesh-relative) and `meshletCount`, a pad word its `triangleCount`.
   - `gpuschema` and the capture's decoders for the new tables; the `memory` record's `geometryPool` with the meshlet pools.
4. **Culling writes task commands.** `instancecull.comp`: an instance's mesh is in the pool when `meshletCount > 0`; the scatter writes
   `{ceil(meshlets / 32), 1, 1, instance}`; primitives counted from triangle counts. `DrawLists`: the command type and stride, `report` and the
   draw log by instance.
5. **Task and mesh shaders, every pass.** `shaders/meshlet.glsl` (structs, pulling, the tests), `meshlet.task` (one shader, a push constant
   names the view, the planes come from the cull parameters; a specialisation by define for the passes that count), `gbuffer.mesh`,
   `depth.mesh`, `depthcutout.mesh`, `shadow.mesh`, `shadowcutout.mesh`, `debugview.mesh`. Fragment shaders keep their inputs. The geometry
   set gains the pools, the meshlet tables, the mesh table and the cull parameters; the shadow set the same without textures. The passes call
   `drawMeshTasksIndirectCount`. The vertex shaders and `debugview.geom` are deleted.
6. **Statistics, the debug view, visibility.** Meshlet statistics (decision 8) in `CullResult`, the `culling` record and the Culling window;
   `debugview meshlet` (a colour per meshlet: instance and meshlet index hashed) and `meshletid` raw value; Engine Map nodes; README
   updates (`src/renderer/README.md` "Scene data on the GPU", "Culling and draw lists", "Passes", "Device requirements", "Known gaps").

## Results (2026-10-22)

Every step landed in one sitting; what each showed:

- **Step 1, packing.** meshoptimizer v1.3 vendored. Sponza: 11.24 M vertices (unwelded) became 2.03 M, 41,012 meshlets for 3.75 M
  triangles (91 a meshlet); `arch_stones_01` 189,159 vertices to 33,465 and 703 meshlets. Sponza's geometry on the GPU went from 674 MB
  (vertices 629 MB, indices 45 MB) to 140 MB (vertices and positions 114 MB, meshlets 27 MB). Packing Sponza twice gives byte-identical packs.
- **Step 2, the RHI.** `ngen-example-meshshader` passes `--check --validation` on RADV and on the lavapipe CI uses (Mesa 26.1.6, run locally
  with its LLVM); every other example unchanged on both; the `indirect` hash made locally equals CI's, so the new hash should hold. Two things
  only validation found: SPIR-V 1.6 shaders need `maintenance4` (core in 1.3, enabled now), and `gl_DrawID` in a task shader needs GLSL 4.60.
- **Steps 3-5, the GPU scene and every pass.** Built as planned. The draw commands are read both as indirect arguments and by the task shader,
  which needed a buffer state of its own (`RhiBufferState::IndirectStorageRead`): the frame graph mapped a combined access to `StorageRead`
  and dropped the indirect stage. three_cubes: the four baselines byte-identical to the indexed path. Sponza: 2 or 3 pixels of 3.7 M differ,
  by at most 3 levels (decision 9: depth ties in another order); two runs of the new code byte-identical.
- **Step 6, counts and the view.** Sponza from `/root/PhysCamera001`: the camera's instances hold 24,130 meshlets, 7,021 culled by the
  frustum and 926 by the cone, 1.47 M of the instances' 2.19 M triangles drawn; cascade 0 2.69 M to 1.65 M, cascade 1 at the default camera
  290 k to 51 k. Test world at dense_forest (826,723 slots): 137,723 meshlets tested, 3.93 M of 4.52 M triangles drawn. `debugview meshlet` and
  `wireframe` draw through the mesh shader; picking names the prims; `renderlod {check: true}` 0 differences of 70,697 groups; layered clean.
- **Found by the occlusion plan's runs**: the `counters` record turns pipeline statistics on, and a query counting input assembly, vertex
  shaders or clipping may not be active while a mesh shader draws. `RhiPipelineStats` keeps fragment and compute invocations only.
- Performance: measured with the occlusion plan, against the commit before both (that plan's Results).

## Visibility

- **Records**: `culling` gains `meshlets` (per view: tested, frustum-culled, cone-culled, drawn, triangles drawn); `memory` lists
  `gpuscene.pool.meshlets`, `gpuscene.pool.meshletvertices`, `gpuscene.pool.meshlettriangles` and the `geometryPool` part their use; `render`'s
  meshes gain meshlet counts; `gpuscene` the meshlet pools.
- **Verbs and RPC**: `debugview meshlet` (and the method it is); no new method otherwise: the counts are records.
- **Windows**: the Culling window's meshlet rows; the debug view; the GPU Scene window's meshlet pool use; the Frame Graph and Render
  Debug windows show the passes as before (their draws are task draws).
- **Tools**: `ngen-packdump` prints meshlet counts per mesh.
- **Engine Map**: "Meshlet build" in the asset server's lane (the USD packer); "Meshlet cull" (task shaders) in the render thread's lane
  between culling and the mesh passes, with its live numbers.
- **Trace**: none per frame; `PipelineCreateFailed` covers a broken mesh shader as any other.

## Verification

- **Packing**: Sponza and the test world pack with no new warnings; `ngen-packdump` on a Sponza mesh shows vertices about a third of
  before (welded) and on average more than 100 triangles per meshlet on large meshes; a repack gives byte-identical packs (determinism).
- **RHI**: `ngen-example-meshshader --check --validation` passes on RADV and on lavapipe; every other example unchanged.
- **Baselines**: the eight screenshots against those taken before the change: identical, or every difference explained (decision 9) and
  the baselines re-taken; runs of the new code byte-identical to each other.
- **No validation messages** on three_cubes, layered, Sponza and the test world at its four viewpoints (`--fail-on-validation`).
- **Counts**: the `culling` record's camera meshlets drawn are fewer than tested on Sponza from inside the atrium and at the test world's
  dense_forest; cone-culled > 0 on Sponza; the triangles drawn per view below the instance-level triangles.
- **Render LOD and picking unchanged**: `renderlod {check: true}` 0 differences at the four viewpoints; `pick` on Sponza names the same prim
  as before.
- **Cost, measured and recorded** (release build): geometry, shadow and prepass GPU zones and the cull passes at Sponza and the test world's
  four viewpoints, before and after; the `memory` record's geometry pool before and after.

## Deferred / follow-ups

- **Decision 4 B** (work items, one draw per region). Trigger: per-draw cost visible in the GPU zones.
- **Compressed vertices and 8-bit triangle streams**. Trigger: the geometry pool's size in `memory` at the test world or Caldera.
- **Mesh shader pipeline statistics** (`VK_EXT_mesh_shader` queries). Trigger: a device that reports them, and the Counters window needing
  them; RADV reports `meshShaderQueries` false.
- **Cluster LOD** (roadmap).
