# Cluster LOD: a pack-time cluster DAG for every mesh, selected per meshlet on the GPU

**Status. Draft.** A prototype run, to see how far it gets: it is built apart from the roadmap's order and its trigger for cluster LOD (decided
2026-11-19). Not a spike: what works is kept. Decisions 1–7 locked 2026-11-19.

*CLOD* here means cluster LOD in the Nanite style. The meshoptimizer helper it builds on uses the same prefix (`clodBuild`, `clodGroup`). Classic
continuous LOD (progressive meshes, vertex splits, geomorphing) was considered and rejected: it fits neither meshlets nor GPU-driven culling.

## Current state

- **Meshlets.** The USD packer welds each mesh and splits each submesh into meshlets of at most 64 vertices and 124 triangles
  (`src/asset/pack/meshletbuild.cpp`, `buildMeshlets`). Each meshlet has a bounding sphere and a normal cone (`PackMeshlet`,
  `src/asset/pack/scenepacktypes.h`). The task shader (`shaders/meshlet.task`) tests each meshlet work item against the frustum, the cone and the
  depth pyramid, and packs the survivors into mesh workgroups. See `src/renderer/README.md`, "Meshlets", and [ADR 64](adr/0064_meshlets_on_mesh_shaders.md).
- **LOD.** UsdLod is the only LOD convention ([ADR 50](adr/0050_usdlod_only_lod_convention.md)). A root with payload levels streams by residency
  ([ADR 70](adr/0070_residency_from_streaming_radii.md)). A root with inline levels is render LOD: `LodSelect` picks the level per group per frame on
  the GPU, from the live main camera ([ADR 68](adr/0068_render_lod_on_gpu.md), [ADR 72](adr/0072_main_camera_decides.md)).
- **What is missing.** A level is one fixed mesh. A large mesh draws all its triangles at any distance, unless meshlet culling removes them. A level
  switch is a pop or a dither fade over a band ([ADR 69](adr/0069_lod_heuristics_and_dither_fade.md)). Nothing reduces detail inside one mesh, and
  ADR 50 records "there is no automatic LOD generation" as a consequence.
- **What exists to build on.** The vendored meshoptimizer is v1.3. It ships `external/meshoptimizer/demo/clusterlod.h`, a cluster DAG builder
  (`clodBuild`, `clodLocalIndices`, `clodBuildHierarchy`), and `meshopt_partitionClusters` and `meshopt_simplifyWithAttributes` in the library.
  Packers already take rule parameters (`--param key=value`, `PackerArgs::param` in `src/asset/pack/packer.h`).

## Scope

In:
- Pack time: every mesh gets a cluster DAG per submesh in place of the flat meshlets. Each cluster is a meshlet with LOD bounds: its own error and
  its parent group's error. One project-wide switch, a parameter of the `usd` pack rule, turns it off.
- Runtime: the task shader rejects the clusters that the error test does not select, before the frustum test. The test uses one eye for every view,
  the main camera's (`RenderSnapshot::lodEye`).
- The whole DAG of a mesh is resident whenever the mesh is. No new residency.
- Tuning, records, RPC, verbs, a debug view and LOD window and Culling window sections (Visibility).

Out:
- Per-mesh control, opting in or out (Deferred / follow-ups).
- Streaming clusters, or pages of clusters. Decision 3 keeps [plan_streaming_lod.md](plan_streaming_lod.md) decision 3 unchanged.
- Replacing authored UsdLod levels. A UsdLod root still picks its level. Cluster LOD refines inside the meshes that the level draws.
- Hierarchical traversal (a BVH over groups, `clodBuildHierarchy`). Selection is a flat test per cluster (decision 4).
- Skinned and deforming meshes: they wait for animation (vertical slice step 6).
- Ray tracing, software rasterization of small triangles, and foliage border dilation (`simplify_dilate_borders`).

## Decisions

### 1. Technique: a cluster DAG built with `clusterlod.h`

**Locked** (2026-11-19). Each submesh's meshlets are grouped by `meshopt_partitionClusters`. Each group is simplified with its border locked, then
split into new meshlets. The process repeats until the mesh stops reducing. Selection is per cluster and needs no neighbour information. Two
clusters that share a border always decide by the same group bounds, so no cracks open. A cut through the DAG replaces regions of the mesh, so the
mesh never switches as a whole.

- **A. `clusterlod.h` as it ships** (chosen): 962 lines, maintained with meshoptimizer, and its author documents it for use "as is". The packer
  includes it from the submodule, so it updates with the submodule.
  - **A known smell, accepted** (2026-11-19). Upstream ships it in `external/meshoptimizer/demo/`, beside the demo program, not in `src/`. The
    meshoptimizer README calls it a companion "single-header C/C++ library for continuous level of detail", distributed alongside the library
    since v1.0, so `demo/` is where upstream puts it, not a sign that it is unsupported. The engine reaches it the way it reaches stb and tinyexr, by
    an include directory, but names the dependency in `build.cpp` so the oddity is visible in one place. It has no API promise: a submodule update
    that changes `clodConfig` or the output callback breaks one file, `clusterlodbuild.cpp`.
- **B. Our own builder on the meshoptimizer primitives**: full control, but it duplicates code that already exists. Revisit only if the packer needs
  something that `clodConfig` cannot express.

### 2. Every mesh gets a DAG; UsdLod still picks the level

**Locked: every mesh** (2026-11-19). The packer builds a DAG for every mesh it packs. A UsdLod root still chooses which level, and so which meshes,
draws. Cluster LOD chooses how much of each drawn mesh's detail draws.

Alternatives considered, kept as the path not taken:
- **A. Opt in per mesh** by a `uniform bool ngen:clusterLod` attribute: the author decides, at the cost of the engine's first `ngen:` attribute.
- **B. Opt in by triangle count**, a pack rule parameter: nothing authored, the engine decides by size.
- **D. On by default, opt out** by the attribute: automatic, with an author override.

Chosen over these because it is the simplest: no attribute, no per-mesh state, and one path that every mesh exercises.

Consequences:
- **ADR 50 is amended.** UsdLod stays the only *authored* LOD convention. "There is no automatic LOD generation" no longer holds: cluster LOD is
  computed for every mesh at pack time. The landing ADR says so.
- **"Authored, not computed" holds where it was made.** That rule governs the streaming structure ([plan_streaming_lod.md](plan_streaming_lod.md)
  decision 3), and decision 3 here leaves it unchanged. Detail inside one mesh is not streaming structure.
- **Authored decimation chains become redundant.** A render-LOD root whose levels are simplified copies of one mesh (the test world's `lod0`–`lod2`
  per prop, Caldera's `modelLod`) still works. Each level gets its own DAG, and the level switch still pops or fades. Content made for the engine
  needs only `lod0`; the guidance goes in `src/world/README.md`.
- **UsdLod keeps what a DAG cannot do.** Payload levels keep far content out of memory. Merged proxies and impostors replace many meshes with one.
  Empty levels drop content. Levels can remove prims or use cheaper materials. Physics and audio are UsdLod domains.
- **Small meshes cost almost nothing.** Inferred from `clodBuild`'s stop rule (`simplify_threshold`): a mesh of one or a few clusters stops at one
  depth, with no extra memory or work items. The cost falls on large meshes, which are the ones that gain. Verification checks this.
- **Pack time grows** for every large mesh on a cold cache. Verification measures it on Sponza and the test world.
- **The switch.** The `usd` rule in `pack.cpp` gets the parameter `clusterLod=on|off`, on by default. Off packs flat meshlets through
  `buildMeshlets`, as today. It is the baseline for comparisons and the way back if pack time or memory hurts. The `asset rules` record shows it.
  **Locked** (2026-11-19): the off path stays after landing, because it is the only way to tell a DAG problem from a meshlet problem.

### 3. The whole DAG is resident

**Locked** (2026-11-19). A mesh's pack holds every DAG level, and the geometry pool loads it whole. Memory is about 1.5× to 2× a large mesh's flat
meshlets. The extra levels share the original vertex buffer, because `clodCluster::indices` refer to it. With every mesh carrying a DAG, this
applies to every large mesh in a scene: the geometry pool's use is measured on the test world (Verification). Streaming clusters is deferred.

### 4. Selection: a flat test per cluster in the task shader

**Locked: A** (2026-11-19). Every cluster of a submesh is a meshlet work item, at every DAG depth. The task shader tests each work item before the
frustum test. With `refined` the group that a cluster was simplified from, a cluster draws if:

```glsl
bool clusterSelected(MeshletLod lod, mat4 model, float scale) {
    // The coarser version (the parent group) is not good enough here.
    bool parentTooCoarse = projectedError(lod.parentCenter, lod.parentRadius, lod.parentError, model, scale) > params.clusterLodThreshold;
    // This cluster is good enough here. Original geometry has error 0 and always qualifies.
    bool selfFineEnough = projectedError(lod.selfCenter, lod.selfRadius, lod.selfError, model, scale) <= params.clusterLodThreshold;
    return parentTooCoarse && selfFineEnough;
}
```

`projectedError` follows `clusterlod.h`: `error / max(distance(center, eye) - radius, zNear) * (proj[1][1] * 0.5)`, in fractions of the view
height, with the sphere and error scaled by the instance model's largest scale. A terminal group has an infinite parent error, so its coarsest
clusters draw at any distance.

- **A. Flat test** (chosen): no new pass or dispatch. Work items grow by the DAG's extra levels (about 1× more for a large mesh), and a rejected
  work item costs about one meshlet test. The existing growth of the work-item buffer handles the size. It keeps the frame's order deterministic,
  and it measures what B would save.
- **B. BVH traversal** (`clodBuildHierarchy`, a persistent-thread or multi-pass traversal before the task shader): it visits only the selected
  region, so it scales to meshes with very many clusters. It adds a pass with a queue, and its order depends on scheduling unless extra work is done.
  A follow-up with a trigger.

### 5. One eye for every view: the main camera's

**Locked** (2026-11-19). The cascades and the camera select clusters from the same eye, `RenderSnapshot::lodEye`. This is the rule that render LOD
already follows ([ADR 68](adr/0068_render_lod_on_gpu.md), [ADR 72](adr/0072_main_camera_decides.md)). The shadow caster is then the same surface
that the camera sees, which prevents self-shadowing acne between two cuts. A debug camera that renders from elsewhere shows the main camera's cut.

Rejected for now: per view (the cascade's own projection). Fewer shadow triangles far away, but the caster and the receiver differ. A follow-up
with a trigger.

### 6. The threshold is in pixels, view-local

**Locked** (2026-11-19). `clusterLodThreshold` is set in pixels at the render target's height and converted to the view-height fraction in the
culling parameters. The default is 1 pixel. It is view tuning, like `lod-bias`: never saved, reachable by RPC and verb.

### 7. LOD data on the GPU: a parallel table at the meshlet pool's positions

**Locked: A** (2026-11-19). Each meshlet in the pool gets a `GpuMeshletLod` at the same pool position, in a fourth buffer of the meshlet pool. An
instance flag, `gpuInstanceClusterLod`, is set when its mesh has more than one DAG depth. The task shader reads the LOD record only for flagged
instances, so a single-depth mesh skips the read and the test.

- **A. Parallel table** (chosen). With every mesh carrying a DAG, every entry is used. The table still pays: the task shader reads the LOD record
  first, and a work item that the test rejects never reads its `GpuMeshlet`. Inferred: about half of a large mesh's work items are rejected,
  because its DAG holds about as many coarse clusters as fine ones. Only the task shader and `debugview.mesh` read `GpuMeshlet`: the mesh shaders
  take their pool offsets from the payload.
- **B. Merge it into `GpuMeshlet`** (80 bytes): one record per work item, but every work item, rejected or not, reads all of it.
- **C. A separate pool with an offset in `GpuMeshEntry`**: no gain once every mesh has the table, and `GpuMeshEntry` would grow from 16 to 32 bytes.

**The record is 48 bytes, in full floats** (locked 2026-11-19): two spheres, two errors and the depth. Half-float errors would fit 32 bytes, but
an error's range spans the mesh's scale down to zero, and a rounded error can break the parent-over-self order that keeps cuts crack-free. Sponza's
41,012 meshlets take about 2 MB of it.

## Steps

### 0. Set up and take the baselines

Done when the run starts, before any code changes.
- The worktree: initialize the submodules, bootstrap `ngen-build`, build `ngen-cli` (`AGENTS.md`, "Build"). Link `assets/main_sponza` and
  `assets/streamtest-world` from the main checkout; Caldera is not linked.
- The crack-test sphere (decided 2026-11-19): `assets/clusterlod/makesphere.py`, plain Python 3 with no OpenUSD, writes
  `assets/clusterlod/sphere.usda` as text. A subdivided icosphere of about 100,000 triangles, closed; two `GeomSubset`s, one per hemisphere, each
  bound to a material of its own, so the DAG's locked submesh border is tested; texture coordinates with a seam along one meridian (split
  vertices), so attribute-aware simplification across a seam is tested. Only the script is committed: `.gitignore` gets `assets/clusterlod/*.usda`
  beside the other generated and downloaded assets. The script takes no arguments and writes the same bytes on every run. A USD `Sphere` prim
  can't stand in: the packer tessellates it at 32 segments (`primshapemesh.cpp`), too few clusters for a DAG.
- The baselines, on the unchanged `HEAD`: Sponza's screenshot at the default camera and its `ngen-packdump` output (Verification 1); the test
  world's `gpuscene` pool use, work-item buffer capacity and GPU frame time at `flight_10`, and the cold-cache pack time of Sponza and the test
  world (Verification 8).

### 1. Pack the DAG

- `build.cpp`, beside the `meshoptimizer` library: a named entry for clusterlod.h, with a comment that says what it is, that upstream ships it in
  `external/meshoptimizer/demo/`, and that only `ngen-packer-usd` uses it. `ngen-packer-usd` adds `external/meshoptimizer/demo` to its `.include`
  list and `src/asset/pack/clusterlodbuild.cpp` to its sources, next to `meshletbuild.cpp`. No other target sees `demo/`; the `meshoptimizer`
  library's `public_include` stays `external/meshoptimizer/src`. If the header raises `-Wall` warnings, they are turned off for that one file
  (`.for_source(…)`, as `usdpacker.cpp` is), not for the target.
- New `src/asset/pack/clusterlodbuild.{h,cpp}`. The `.cpp` defines `CLUSTERLOD_IMPLEMENTATION` itself, before it includes `clusterlod.h`; it is
  the only file that includes it:

  ```cpp
  // Builds each submesh's cluster DAG with clusterlod and fills the mesh's meshlets with every cluster of every depth, finest first, and
  // meshletLods in parallel. A submesh's depth-0 clusters (its original geometry) come first in its range, originalMeshletCount of them.
  auto buildClusterLod(MeshPack& mesh) -> void;
  ```

  - `clodDefaultConfig(meshletMaxTriangles)`, then `max_vertices = meshletMaxVertices`, so DAG clusters fit the mesh shader as meshlets do.
  - One `clodBuild` per submesh, so a cluster never mixes materials. Vertices that a submesh shares with another submesh are locked
    (`clodMesh::vertex_lock`), so submesh borders stay closed.
  - Attribute-aware simplification on the normal and the texture coordinate (`vertex_attributes` over `PackVertex`, weights in the builder,
    named constants).
  - In the output callback, each cluster becomes a `PackMeshlet` through `clodLocalIndices`, `meshopt_optimizeMeshlet` and
    `meshopt_computeMeshletBounds` (the culling sphere and cone, as today). Each cluster also gets a `PackMeshletLod`.
- `src/asset/pack/scenepacktypes.h`:

  ```cpp
  // Cluster LOD bounds of one meshlet, in the mesh's space: its own simplification error and that of the group it would be replaced by.
  // A meshlet of original geometry has selfError 0; a meshlet of a terminal group has parentError FLT_MAX.
  struct PackMeshletLod {
      std::array<float, 3> selfCenter = {0.0f, 0.0f, 0.0f};
      float selfRadius = 0.0f;
      float selfError = 0.0f;
      std::array<float, 3> parentCenter = {0.0f, 0.0f, 0.0f};
      float parentRadius = 0.0f;
      float parentError = 0.0f;
      uint32_t depth = 0; // the DAG depth the meshlet was made at, 0 for original geometry
  };
  ```

  `MeshPack` gains `std::vector<PackMeshletLod> meshletLods` (one per meshlet, empty when packed with `clusterLod=off`) and `uint32_t
  clusterLodDepths` (0 when off, 1 for a mesh that did not reduce). `PackSubmesh` gains `uint32_t originalMeshletCount`: the depth-0 clusters at
  the start of its meshlet range (equal to `meshletCount` when off).
- Versions, both bumped in this change, as the meshlet change (`40bb2b3`) bumped both: the `usd` rule in `pack.cpp` from 11 to 12, with
  `.params = {{"clusterLod", "on"}}`; `scenePackFormatVersion` (`src/asset/pack/scenepacktypes.h`) from 5 to 6. The rule version makes the asset
  server pack again (its job key hashes it); the format version makes a reader that opens an old file directly, such as `ngen-packdump`, refuse it.
- `src/apps/packerusd.cpp` reads `args->param("clusterLod", "on")` and passes it to the USD packer (`src/asset/pack/usd/usdpacker.cpp`), which
  calls `buildClusterLod` when on and `buildMeshlets` when off. A trace event `ClusterLodBuilt` gives the mesh, its submeshes, depths, clusters
  per depth, triangles at depth 0 and at the coarsest depth, and milliseconds.
- `src/asset/pack/scenepack.cpp`: write and read `meshletLods`; validate one entry per meshlet or none, `selfError <= parentError`, and `depth <
  clusterLodDepths`.
- `src/apps/packdump.cpp`: per mesh, its depths, meshlets and triangles per depth, and the memory factor over depth 0; for the pack, the meshes by
  number of depths and the total memory factor.

### 2. Upload the LOD records

- `src/renderer/gpuscene.h`: `GpuMeshletLod` (std430) and the `gpuInstanceClusterLod` flag (bit 0, free) in `GpuInstanceRecord::flags` and
  `GpuCullRecord::flags`, set when the mesh's `clusterLodDepths > 1`.

  ```cpp
  // One meshlet's cluster LOD bounds in the meshlet LOD pool (std430, stride 48), at the meshlet's own pool position: PackMeshletLod in
  // full floats. parentError is FLT_MAX for a terminal group's meshlet.
  struct GpuMeshletLod {
      glm::vec3 selfCenter = glm::vec3(0.0f);
      float selfRadius = 0.0f;
      glm::vec3 parentCenter = glm::vec3(0.0f);
      float parentRadius = 0.0f;
      float selfError = 0.0f;
      float parentError = 0.0f;
      uint32_t depth = 0;
      uint32_t pad = 0;
  };
  static_assert(sizeof(GpuMeshletLod) == 48);
  ```
- The geometry pool's meshlet part gets the parallel buffer, allocated and freed with the meshlets. `PoolUse` counts it. A mesh packed with
  `clusterLod=off` leaves its entries unused.
- The geometry descriptor set gains the buffer, read by `meshlet.task` and `debugview.mesh`. `gpuschema.cpp` gets its schema, for `capture` and
  `dump-gpuscene`.

### 3. Select in the task shader

- `shaders/meshlet.glsl`: `MeshletLod`, `projectedError` and `clusterSelected` (decision 4). The culling parameters gain `lodEye`, `projY`,
  `zNear` and `clusterLodThreshold`, plus a flag for cluster LOD off.
- `shaders/meshlet.task`: for an instance with `gpuInstanceClusterLod`, a work item that is not selected does not survive. It is counted as
  `lodRejected`, never as culled, before the frustum test. With cluster LOD off at runtime, only depth-0 clusters are selected (the original
  geometry).
- Classify mode (`meshletcull`): rejected clusters are left out, as the levels that render LOD does not draw are left out.
- `MeshletCounts` (`src/renderer/drawlists.h`) gains `lodRejected`. A per-depth histogram of drawn meshlets, up to 16 depths, is counted for the
  camera's geometry pass only.
- `CullResult::sceneMeshlets` counts every cluster of every live instance, as it is `DrawLists::liveMeshletCount`, the CPU count that also sizes
  the work-item buffer (`ensureWorkCapacity(liveMeshlets() * 2, …)` in `renderer.cpp`): every DAG cluster is a work item, so a depth-0 count would
  size the buffer too small. A new `CullResult::sceneOriginalMeshlets` counts the depth-0 clusters, kept on the CPU beside it from
  `PackSubmesh::originalMeshletCount` through `GpuInstance` (the CPU struct; `GpuCullRecord` stays 48 bytes). `sceneTriangles` needs no change:
  it is `indexCount / 3`, and the index buffer is the original mesh.
- `GpuMeshEntry::triangleCount` stays `meshletTriangleCount`, the mesh's pool allocation, which now spans every cluster; its comment changes from
  "the mesh's triangles" to say so. No shader reads it.
- The work-item buffer holds twice every cluster: about 4× a large mesh's original meshlets. Its capacity is compared against step 0's
  baseline. No record shows it by that name today: read it from the `memory` record if that lists the buffer, otherwise add `workItemCapacity`
  to the `culling` record. Sizing it to the meshlets drawn is the existing roadmap follow-up, taken up only if the size hurts.

### 4. Tuning, records, the debug view and windows

- `src/view/lodcommands.cpp`: `view.clusterLod.tuning {enabled, thresholdPixels}`, and the verbs `clusterlod on|off` and `clusterlod-threshold
  <px>`. Both reach `RenderSnapshot`, as `lod-bias` does.
- The `culling` record's meshlet counts gain `lodRejected` and the per-depth histogram. A `clusterLod` object in the `lod` record holds the tuning,
  meshes by number of depths, resident clusters per depth, and the pool memory of the LOD records.
- A `clusterlod` debug view (`debugview.mesh`): every drawn meshlet coloured by its DAG depth, on the `lodlevel` ramp, with a meshlet-edge tint.
- The LOD window gets a Cluster LOD section: on and off, threshold, and the drawn-per-depth histogram. The Culling window gets the
  LOD-rejected column.

### 5. Documentation

- `src/renderer/README.md`: a "Cluster LOD" section after "Render LOD", which says where selection sits in the frame (the task shader, after
  `LodSelect` and instance culling, before the frustum test).
- `src/asset/README.md`: the DAG in the mesh pack, the `clusterLod` parameter, and the memory factor.
- `src/world/README.md`: what UsdLod levels are for now that every mesh has a DAG (decision 2's consequences): structure, memory and other
  domains, not decimation of one mesh.
- `GLOSSARY.md`: *cluster LOD*, *DAG depth* (not a UsdLod *level*: the glossary's *level* entry names both), *error threshold*, and *cluster*
  (a meshlet with LOD bounds; with `clusterLod=on`, every meshlet).
- When it lands: an ADR "Cluster LOD for every mesh" that amends ADRs 50 and 64, and its row in `docs/adr/README.md`.

## Visibility

- **Records.** `culling`: `meshlets.lodRejected`, `meshlets.drawnByDepth[]`, `sceneMeshlets` (now every cluster), `sceneOriginalMeshlets`. `lod`: `clusterLod {enabled,
  thresholdPixels, meshesByDepths[], residentByDepth[], lodRecordBytes}`. `gpuscene`: the pool's LOD record use. `asset rules`: the `usd` rule's
  `clusterLod` parameter.
- **RPC and verbs.** `view.clusterLod.tuning`; `clusterlod on|off`, `clusterlod-threshold <px>`; `debugview clusterlod`.
- **Trace.** `ClusterLodBuilt` per mesh in the asset server's trace.
- **Windows.** The LOD window's Cluster LOD section, the Culling window's LOD-rejected column, and the `clusterlod` debug view. `ngen-packdump`
  shows the DAG per mesh.
- **Where in the flow.** The mesh pack (pack time) → the geometry pool (upload) → `LodSelect` (the UsdLod level) → instance culling → the task
  shader (cluster selection, then frustum, cone and occlusion) → the mesh shaders. The renderer README's section states this.
- **Gaps, named.** There is no per-instance view of the cut, only per-meshlet colours and totals: add one if debugging a single mesh needs it. The
  `clusterLod` pack parameter changes only by editing `pack.cpp` and restarting the asset server, as every rule parameter does.

## Verification

Test assets: Sponza (`assets/main_sponza/`, git-ignored), the streaming test world, and a closed, finely tessellated sphere that fills the view
(`assets/clusterlod/sphere.usda`, generated by `makesphere.py` in step 0, git-ignored).

1. **Off is unchanged.** With `clusterLod=off` in `pack.cpp`, Sponza at the default camera: the screenshot is byte-identical to one taken before
   the change, and `ngen-packdump` prints the same meshlets per mesh.
2. **The DAG packs.** With `clusterLod=on`, `ngen-packdump` on Sponza shows for every mesh: depth 0's triangles equal the mesh's triangles. A mesh of
   one cluster has one depth. A mesh with more depths has fewer triangles at each depth. The trace has one `ClusterLodBuilt` per mesh and no
   warnings.
3. **Depth 0 is the mesh.** With `clusterLod=on` and `clusterlod off` at runtime, Sponza at the default camera: `culling.sceneTriangles` equals the
   value with `clusterLod=off`, and at most 0.01% of pixels differ from the off screenshot (depth ties from another meshlet order, as in
   [plan_meshlets.md](plan_meshlets.md)).
4. **No cracks.** The sphere with the camera outside it, at thresholds of 1, 8, 32 and 128 pixels: the screenshot has no sky pixels inside a
   rectangle that lies within the sphere's silhouette at every threshold.
5. **Detail falls with distance.** Sponza at the default camera, `clusterlod off`, then `clusterlod on` at 1 pixel, then at 8 pixels:
   `culling.triangles` falls strictly at each step, and `meshlets.lodRejected` is 0 when off and above 0 when on.
6. **Counts agree.** In every recorded frame, the sum of `meshlets.drawnByDepth` equals the camera's `meshlets.drawn`.
7. **One eye.** With the debug camera placed 100 m away and rendered from, each cascade's drawn triangles are unchanged from the frame before it
   was placed (the main camera did not move).
8. **The test world fits.** A flight over the test world (`fly /cameras/flight_ 30`) with `clusterLod=on` logs no `GeometryPoolFull` warning at
   the default pool limit. Recorded and reported, not pass or fail: the `gpuscene` pool use, the GPU frame time at `flight_10`, and cold-cache
   pack time of Sponza and the test world, each with `clusterLod=on` and `off`.
9. **Image quality at 1 pixel.** Against `clusterlod off` at the same camera, the share of pixels that differ by more than a set number of levels is
   below a set share. Both numbers are set at the first measurement (Open questions).
10. **Visibility.** The records, RPC method, verbs, debug view and window sections from Visibility each answer, through `./ngen-cli introspect`
    and a `--show-ui` screenshot.

## Open questions

- Verification 9's thresholds: set from the first measurement on Sponza.
- Whether `simplify_permissive` with `attribute_protect_mask` gives fewer stuck groups on Sponza's UV seams than the default.

## Deferred / follow-ups

- **clusterlod.h out of `demo/`.** Include it from a supported location, or replace it with our own builder (decision 1, B). Trigger: upstream
  moves it (into `src/` or a repository of its own), or a submodule update breaks its API in a way that a small edit to `clusterlodbuild.cpp`
  cannot absorb.

- **Per-mesh control** (decision 2, options A and D). Trigger: a mesh where the DAG visibly hurts, which the pack-wide switch cannot fix without
  losing cluster LOD everywhere.
- **Streaming clusters or pages.** Trigger: the GPU geometry budget is exceeded by resident DAGs, or a world needs more detail than fits. It would
  replace [plan_streaming_lod.md](plan_streaming_lod.md) decision 3 for clusters, and needs GPU-side requests in place of position-based residency.
- **BVH traversal** (decision 4, B). Trigger: the cost of LOD-rejected work items in the task shader exceeds 0.5 ms on a test scene, or one mesh has
  more than about 10,000 clusters.
- **Per-view selection** for the cascades (decision 5). Trigger: shadow triangles dominate a frame with cluster LOD on.
- **Small triangles.** A compute rasterizer for clusters whose triangles are smaller than a pixel. Trigger: such clusters measurably cost the
  hardware rasterizer.
- **Foliage.** `simplify_dilate_borders` for open meshes such as leaves. With every mesh simplified, this is more likely to be needed. Trigger:
  foliage thins visibly with cluster LOD on.
- **Skinned meshes.** Trigger: animation (vertical slice step 6) with dense skinned meshes.
- **Decimation-chain findings.** An authoring finding (as the `streaming` record's `findings`) for render-LOD roots whose levels only simplify one
  mesh. Trigger: content authored for the engine that still carries such chains.
- **Generating UsdLod levels from the DAG**, for tools that read UsdLod. Trigger: a tool or exporter that needs the engine's LOD as USD.
