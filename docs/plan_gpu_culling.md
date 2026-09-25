# GPU culling

**Status. Landed.**

Stage 5 of [plan_gpu_driven.md](plan_gpu_driven.md).

## Current state

After stage 4, every mesh draw is a `drawIndexedIndirectCount` per view and bucket (`docs/plan_indirect_draws.md`). The producer is still the CPU:

- **Main thread.** `cullInstances` and `cullShadowCascades` (`src/renderer/culling.cpp`) test each instance's `RenderMeshInstance::worldBounds`
  against `Frustum::fromViewProj` planes (camera, or the frozen camera, and each cascade). The results go into `RenderSnapshot::visible`,
  `shadowVisible`, `culledInstances` and `shadowCulled`.
- **Render thread.** `DrawLists::build` turns the masks into commands and counts in host-visible per-slot buffers, keeping a CPU copy that the
  draw log and the per-pass `draws`/`primitives` read.
- **Editor.** The Culling window's counts (`setCullStats`, `setShadowCullStats`) and the red/green AABB overlay (`EditorUI::drawDebug`, reading
  `visible`) come from the main-thread masks.

Reference numbers to match, from stage 4 at the Sponza courtyard camera: 407 instances, `culled` 342, `shadow_culled` 141 over three cascades;
`GeometryPass` 65 draws, `ShadowPass` 204.

Per-instance data on the GPU: the instance record (model matrix, material), the material table and the geometry pool. The GPU has no instance
bounds, no mesh ranges and no draw flags (`primFirst`, `doubleSided`).

## Scope

**In**

- An `InstanceCull` compute pass that tests every instance against every view and writes the compacted commands and counts that the stage 4 passes
  already read.
- The per-instance data culling needs, on the GPU.
- A visibility readback for the editor, the stats and the draw log, one to a few frames late.
- Deleting the CPU culling and the CPU command builder, after recording the reference numbers above.

**Out**

- Occlusion culling (HZB), meshlets, LOD. Umbrella Deferred.
- Async compute. The pass runs on the graphics queue before the shadow pass.
- Rebuilding per-draw timing and GPU-side draw statistics. That is the debugging and introspection pass that follows this stage.
- Moving cascade fitting to the GPU. It is a handful of matrices per frame, computed on the main thread from the camera and scene bounds, and it
  doesn't depend on culling.

## Decisions

Locked after discussion.

1. **Ordered compaction with reduce-then-scan.** Three compute dispatches, graph passes `InstanceCull`, `InstanceCullScan` and `InstanceCullScatter`:
   1. **Cull and count** (one thread per instance, 256 per workgroup). Tests the instance against every view, stores its visibility bits, and
      writes per-workgroup counters for each region.
   2. **Scan** (one workgroup). An exclusive prefix sum over the per-workgroup counts gives each workgroup's start offset per region, plus the
      totals.
   3. **Scatter** (one thread per instance). A shared-memory prefix sum within the workgroup, per region; each command is written at its
      workgroup's start offset plus the local position.

   Instance order is kept, so output is deterministic and screenshots stay byte-comparable. It scales to millions of instances without
   replacement; the scan loops over workgroups, so 1M instances is 4,096 per region.

   Rejected:
   - Atomic append (the common engine choice). Nondeterministic order changes pixels wherever depths tie, and would force tolerance-based image
     checks.
   - A single-workgroup compaction. It doesn't scale past tens of thousands of instances.
   - Decoupled lookback. It relies on forward-progress guarantees between workgroups that not every GPU gives.

   Async compute stays Out, with the umbrella trigger. The cull passes are separate graph passes with explicit buffer dependencies, so a future
   multi-queue scheduler can move them.
2. **Instance record grows to 112 bytes, plus a GPU mesh table.** The record is `mat4 model`, `uvec4 (material, mesh, indexOffset, indexCount)`,
   `vec3 boundsMin` with `uint flags` (`primFirst`, `doubleSided`, `boundsValid`), and `vec3 boundsMax` with a pad word. The bounds are copied from
   `RenderMeshInstance::worldBounds`, the CPU test's input, and ride the delta upload. `GpuMeshEntry { firstIndex, vertexOffset, indexCount, pad }`
   per mesh index is uploaded in bulk with the pool.
3. **Output buffers imported per frame slot, device-local.** No graph transients yet: the cross-frame sync problem from stage 1 still stands.
   Transients wait for a variable-size per-frame buffer or memory pressure. The umbrella's "transient graph buffers" line for stage 5 moves to its
   Deferred list.
4. **Readback of counters, visibility and (render debugger only) commands**, parsed after the slot's fence.
   - Pass 1 also counts per region the primitives (sum of `indexCount / 3`), and the statistics the CPU produced: camera culled over all instances,
     and per cascade culled and drawn over `primFirst` instances. Pass 2 totals them. So the per-pass `draws`/`primitives`, `culled` and
     `shadow_culled` come from one small counter buffer, and they need the command readback only for the draw log.
   - Everything the editor and dumps show is one frame-slot cycle late.
5. **CPU culling and the CPU command builder are deleted.** `CullState` keeps the enabled/frozen toggles, and the snapshot carries the culling
   view-projection (live or frozen) and the enabled flag.

## Steps

1. **Instance record and mesh table** (`gpuscene.h/.cpp`, the three vertex shaders' `Instance` struct). `GpuInstanceRecord` becomes 112 bytes.
   `updateInstances` takes the world bounds, and the delta upload writes them. `rebuildGeometry` also uploads `GpuMeshEntry { firstIndex,
   vertexOffset, indexCount, pad }` per mesh index.
2. **Cull parameters** (per slot, host-visible). The number of views, and per view the six planes from `Frustum::fromViewProj`, computed on the CPU
   exactly as today. Also an enabled flag (culling off means everything is visible), and the region layout (the `DrawLists` constants).
3. **`shaders/instancecull.comp`**, one shader with a `mode` push constant (0 cull, 1 scan, 2 scatter), `local_size_x = 256`.
   - Membership: camera regions hold every instance with a mesh in the pool; cascade regions hold only `primFirst` ones. Bucket is `doubleSided`.
   - The visibility test is the same positive-corner test as `Frustum::contains`, on planes the CPU computes with `Frustum::fromViewProj`.
4. **`InstanceCullPass`** (`src/renderer/passes/instancecullpass.h/.cpp`). Three graph passes: cull `dispatch(groups)`, scan `dispatch(1)`, and
   scatter `dispatch(groups)`. Buffer barriers come from the declared accesses.
   - The graph orders it after `InstanceUpload` and before the shadow, prepass and geometry passes, which read commands and counts with
     `IndirectRead`. The existing buffer barriers cover `StorageWrite` to `IndirectRead`.
5. **`DrawLists` becomes buffer ownership.** Device-local `Storage | Indirect` command and count buffers, a visibility buffer, and a host-visible
   readback buffer per slot, plus region offsets. A `CullReadback` copy pass after the draw passes, and a parse step after the slot's fence (next to
   `readGpuTimings`) that fills the counts, visibility and optional command list.
6. **Consumers of the readback.**
   - `RenderStats.culled`/`shadow_culled`.
   - The per-pass `draws`/`primitives` and the draw log keep going through `addIndirectStats` and `logDraw` at record time. They are fed from the
     latest readback instead of a CPU list (`DrawLists::report`), so they lag like everything else.
   - The render thread passes a visibility mask to the main thread for the editor (`EditorUI::setCullStats`, `drawDebug`'s overlay), next to the
     render debug snapshot.
7. **Delete the CPU path** (decision 5). Remove the culling calls in `main.cpp` and the snapshot fields.
8. **Observation.** `CullReadback { frame, camera_visible, camera_culled, cascade_culled_0..3 }` each time a readback is parsed, and an
   `InstanceCull` GPU zone (automatic, as a pass).

## Verification

Baseline: the stage 4 screenshots (byte-identical to the pre-stage-1 ones), the stage 4 dump, and the reference numbers in Current state.

- Screenshots byte-identical, prepass off and on. **Verified**: all six pairs identical on the first run. Ordered compaction kept draw order.
- `--fail-on-validation` exits 0. **Verified** on every run, and all 15 RHI examples pass.
- Sponza at the courtyard camera, steady state. **Verified**:
  - `RenderStats.culled` is 342 and `shadow_culled` is 141 (89, 44 and 8 per cascade in `CullReadback`), equal to the CPU reference.
  - `GeometryPass` has 65 draws and `ShadowPass` 204, with the same primitives (447,895 and 6,702,395).
  - The draw log is identical to stage 4, entry for entry.
  - Indirect calls are now 2 and 6, since every bucket issues a call and the GPU count decides.
- `cull off`. **Verified**: `culled` and `shadow_culled` are 0, `GeometryPass` has 407 draws and `ShadowPass` 345.
- `cull freeze` at the courtyard camera, then `camera-frame scene`. **Verified**:
  - `culled` stays 342, where the live framed view culls differently.
  - The screenshot shows only the courtyard-visible part of the building (one end wall, no roof) inside the yellow frozen frustum, against the
    full building with the roof in the unfrozen framed shot.
- `--dump-profile`. **Verified**: no `Cull`, `CullShadow` or `BuildDrawLists` zone. `InstanceCull`, `InstanceCullScan`, `InstanceCullScatter` and
  `CullReadback` appear as passes.
- Moved bounds. **Verified**: `translate /World/Cube_1 0,0,1000` on frame 70 takes three_cubes from `culled` 0 (frame 60) to 1 (frame 120).
- GPU time on Sponza. **Verified**:
  - The three cull passes together take 0.015 ms (0.0055, 0.0019 and 0.0073 ms), and the readback copy 0.003 ms.
  - `ShadowPass` measured 3.45 ms against 3.63 and `GeometryPass` 1.22 against 1.26, within run-to-run noise.
- Not UI-checked: the red/green AABB overlay (`cull show`) reads the readback visibility now. The counts that drive it are verified above.

## Deferred / follow-ups

- Multi-workgroup ordered compaction (decoupled lookback). Trigger: instance counts where the single workgroup shows up in the `InstanceCull` time.
- Transient graph buffers with cross-frame sync. Trigger: in decision 3.
- The debugging and introspection pass (umbrella). Trigger: this stage lands.
- Occlusion culling, meshlets: umbrella Deferred.
