# Meshlets: what the `meshlets` branch learned, for a reimplementation

A record of the `meshlets` branch (October–November 2026), meshlets only: packing, the RHI, the GPU scene, culling into meshlet work, the
task and mesh shaders, the passes, the tools, and what was measured. Occlusion culling (the depth pyramid, the two-phase history, the
culling-camera depth) was built on the same branch on top of this; it is left out here except where it constrains a meshlet decision, and
"Occlusion, left out" says what to drop. The branch's plans are `docs/plan_meshlets.md`, `docs/plan_meshlet_culling_visibility.md` and
`docs/plan_subset_bounds.md`; this document supersedes them as a guide and records what they don't (the rebase onto main, the debug camera,
the release measurements).

## The result in one paragraph

Meshes are welded and split into meshlets (at most 64 vertices, 124 triangles) at pack time with meshoptimizer. Every mesh pass draws with
task and mesh shaders, pulling vertices from storage buffers: no index buffer, no vertex input. The cull compute pass culls instances (frustum,
render LOD) and lays out, per region (view × bucket), every drawn instance's meshlets as work items `{instance, meshlet}` in instance order;
each region is drawn with one `drawMeshTasksIndirect` whose task workgroups each take 32 work items, frustum- and cone-cull them, compact the
survivors in order and launch a mesh workgroup per survivor. At the test world's `flight_10` (release, AMD 890M), meshlets without occlusion
ran 44 ms against main's 49 ms, the geometry pass 21.6 ms → about 7 ms; occlusion culling as first built added 23 ms back (see "Performance").
After the performance work in section 9b (cheaper culling, occlusion in the geometry pass, packed meshlets), the same viewpoint with occlusion
on runs at 15.3 ms: about a third of main's frame.

## 1. Packing

### Weld first

The USD packer emitted three fresh vertices per triangle (`polygonMesh`), so meshlets (bounded by 64 unique vertices) would hold about 21
triangles instead of about 124: six times the meshlets, three times the vertex memory. Weld every mesh in the packer:

```cpp
auto weldMesh(MeshPack& mesh) -> void {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return;
    }
    std::vector<uint32_t> remap(mesh.vertices.size());
    auto uniqueCount = meshopt_generateVertexRemap(remap.data(), mesh.indices.data(), mesh.indices.size(), mesh.vertices.data(), mesh.vertices.size(), sizeof(PackVertex));
    std::vector<PackVertex> welded(uniqueCount);
    meshopt_remapVertexBuffer(welded.data(), mesh.vertices.data(), mesh.vertices.size(), sizeof(PackVertex), remap.data());
    meshopt_remapIndexBuffer(mesh.indices.data(), mesh.indices.data(), mesh.indices.size(), remap.data());
    mesh.vertices = std::move(welded);
}
```

- The remap compares the whole 44-byte `PackVertex`, so only bitwise-identical vertices merge: hard edges keep their split normals.
- Index order is kept, so submesh index ranges don't move.
- Sponza: 11.24 M vertices → 2.03 M. GPU geometry 674 MB (vertices 629, indices 45) → 140 MB (vertices and positions 114, meshlets 27).

### Build meshlets per submesh

Per submesh (a meshlet never mixes materials): `meshopt_optimizeVertexCache` on the submesh's index range, `meshopt_buildMeshlets` (64, 124,
cone weight 0.25), then per meshlet `meshopt_optimizeMeshlet` and `meshopt_computeMeshletBounds`. The code is `src/asset/pack/meshletbuild.cpp`
on the branch (68 lines); its shape:

```cpp
for (auto& submesh : mesh.submeshes) {
    submesh.meshletOffset = (uint32_t) mesh.meshlets.size();
    submesh.meshletCount = 0;
    if (submesh.indexCount < 3) {
        continue;
    }
    std::span<uint32_t> indices(mesh.indices.data() + submesh.indexOffset, submesh.indexCount);
    meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), mesh.vertices.size());
    auto maxMeshlets = meshopt_buildMeshletsBound(indices.size(), meshletMaxVertices, meshletMaxTriangles);
    std::vector<meshopt_Meshlet> built(maxMeshlets);
    std::vector<uint32_t> vertices(indices.size());
    std::vector<unsigned char> triangles(indices.size());
    auto count = meshopt_buildMeshlets(built.data(), vertices.data(), triangles.data(), indices.data(), indices.size(), positions,
                                       mesh.vertices.size(), sizeof(PackVertex), meshletMaxVertices, meshletMaxTriangles, coneWeight);
    for (size_t i = 0; i < count; i++) {
        // meshopt_optimizeMeshlet, meshopt_computeMeshletBounds, then append to mesh.meshlets / meshletVertices / meshletTriangles
    }
    submesh.meshletCount = (uint32_t) mesh.meshlets.size() - submesh.meshletOffset;
}
```

- Why 64/124: meshoptimizer's recommendation for both vendors; a 64-thread mesh workgroup does one vertex and up to two triangles per thread.
  RADV reports `maxMeshOutputVertices` 256 and `maxPreferredMeshWorkGroupInvocations` 128.
- `meshopt_optimizeVertexCache` reorders the submesh's indices in place: the CPU's index list changes order (harmless; it stays the source).
- Deterministic: packing Sponza twice gives byte-identical packs.
- Results: Sponza 41,012 meshlets for 3.75 M triangles (91 a meshlet); `arch_stones_01` 189,159 vertices → 33,465, 703 meshlets.
- The test world's foliage and grass are tiny meshes: one meshlet of about 20 triangles each. That shapes the performance (section 9).

### The pack format

`scenepacktypes.h`:

```cpp
inline constexpr uint32_t meshletMaxVertices = 64;
inline constexpr uint32_t meshletMaxTriangles = 124;

struct PackMeshlet {
    uint32_t vertexOffset = 0;   // into MeshPack::meshletVertices
    uint32_t triangleOffset = 0; // into MeshPack::meshletTriangles
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
    std::array<float, 3> center = {0.0f, 0.0f, 0.0f}; // bounding sphere, mesh space
    float radius = 0.0f;
    std::array<int8_t, 4> cone = {0, 0, 0, 0}; // axis xyz and cos(half angle), each value * 127 (meshopt's s8 form; cutoff 127 culls nothing)
};
static_assert(sizeof(PackMeshlet) == 36); // written raw
```

- `MeshPack` gains `meshlets`, `meshletVertices` (`uint32_t`, mesh-local vertex indices), `meshletTriangles` (`uint32_t` per triangle:
  `i0 | i1 << 8 | i2 << 16`, three meshlet-local indices). One 32-bit word per triangle is one read in the shader, 33% larger than packed
  bytes; compressing it is deferred.
- `PackSubmesh` gains `meshletOffset`, `meshletCount` (the submesh record grows 12 → 20 bytes).
- Three sections: `MeshMeshlets = 15`, `MeshMeshletVertices = 16`, `MeshMeshletTriangles = 17`. Format version 4 → 5.
- The reader validates everything the GPU will trust: submesh meshlet ranges inside the meshlets, each meshlet's counts ≤ 64/124, its vertex
  and triangle ranges inside their arrays, every local index < its vertex count, every meshlet vertex < the mesh's vertex count. A broken pack
  never reaches a shader.
- The index list stays in the pack: it is what meshlets are built from and what the CPU uses (subset bounds, picking, ground following).
- Bump the `usd` rule's version in `pack.cpp` so caches repack. On the branch that was 8 → 9; after rebasing onto main (which had moved to 10)
  it became 11. On a new branch: main's version + 1.
- `ngen-packdump` prints vertices, triangles, meshlets and triangles per meshlet per mesh: useful to see welding and meshlet fill.

### Build plumbing

- `external/meshoptimizer` submodule (v1.3, `https://github.com/zeux/meshoptimizer.git`).
- `build.cpp`: `auto meshoptimizer = cxx::static_library("meshoptimizer").sources(glob({.include = "external/meshoptimizer/src/*.cpp"})).public_include({"external/meshoptimizer/src"});`, linked by `ngen-packer-usd` only.
- `meshletbuild.{h,cpp}` lives in `src/asset/pack/` (C++23), outside the USD file, called from `packMesh` after the mesh is built.
- The shader packer: `.task` and `.mesh` in the `shader` rule's extensions (rule version bump), and glslc with `--target-env=vulkan1.3` for
  those two (`GL_EXT_mesh_shader` needs SPIR-V 1.4; vulkan1.3 gives 1.6). In `src/apps/packershader.cpp`:
  `if (args->source.ends_with(".task") || args->source.ends_with(".mesh")) { glslc.emplace_back("--target-env=vulkan1.3"); }`

## 2. The RHI

- `RhiShaderStage::Task = 1 << 4`, `Mesh = 1 << 5`. Map them everywhere stages are mapped: shader module creation, descriptor binding stages,
  `pushConstants` (`VK_SHADER_STAGE_TASK_BIT_EXT`, `VK_SHADER_STAGE_MESH_BIT_EXT`).
- `RhiGraphicsPipelineDesc` gains `taskShader`, `meshShader`. A pipeline is either vertex (+ geometry) + fragment, or task (optional) + mesh +
  fragment. A mesh pipeline passes `pVertexInputState = nullptr` and `pInputAssemblyState = nullptr`. The branch built the stage list as a
  `std::vector<VkPipelineShaderStageCreateInfo>` with an `addStage` lambda.
- `RhiDrawMeshTasksIndirectCommand {groupCountX, groupCountY, groupCountZ}` (12 bytes, matches Vulkan and D3D12).
- `RhiCommandBuffer::drawMeshTasksIndirect(commands, offset, drawCount, stride)` and `drawMeshTasksIndirectCount(commands, offset, stride,
  count, countOffset, maxDrawCount)`, through `vkCmdDrawMeshTasksIndirect(Count)EXT` loaded with `vkGetDeviceProcAddr` (fail `init` if null).
  The final design uses only `drawMeshTasksIndirect` with one draw per region; the `Count` variant was for the per-instance design (section 4).
- `RhiDeviceLimits`: `maxMeshOutputVertices`, `maxMeshOutputPrimitives`, `maxTaskPayloadSize` from `VkPhysicalDeviceMeshShaderPropertiesEXT`.
- Device creation, all required (fail with a clear message):
  - `VK_EXT_mesh_shader`, features `taskShader`, `meshShader`.
  - `maintenance4` (core in 1.3, but must be enabled): SPIR-V 1.6 shaders declare their workgroup size as `LocalSizeId`. Only validation found it.
  - `shaderDrawParameters` (1.1), for `gl_DrawID` in a task shader. Only needed by the per-instance design; harmless to keep.
  - `shaderSampledImageArrayNonUniformIndexing` (1.2): one draw spans many materials (section 4), so the texture index is non-uniform.
- Barrier stages: `ShaderReadOnly` textures and `UniformRead`/`StorageRead`/`StorageWrite` buffers must include
  `VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT`.
- **A new buffer state, `IndirectStorageRead`**: the region's draw record (`RegionWork`) is read as indirect arguments and by the task shader
  as storage. The frame graph mapped the combined access to `StorageRead` and dropped the indirect stage; the state is
  `VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT` at
  `DRAW_INDIRECT | TASK_SHADER | MESH_SHADER | COMPUTE_SHADER`. Passes declare `FgAccessFlags::IndirectRead | FgAccessFlags::StorageRead`.
- **Pipeline statistics**: a query counting input assembly, vertex shader invocations or clipping may not be active while a mesh shader
  draws. `RhiPipelineStats` keeps fragment and compute invocations only (the `counters` record found it). RADV reports `meshShaderQueries`
  false, so mesh shader statistics are deferred.
- `ngen-example-meshshader`: a task shader culls half a grid of quads by a push-constant plane, the mesh shader draws the rest with
  per-primitive colours; `--check --validation` passes on RADV and lavapipe. The examples' `shadercompile.h` maps Task and Mesh to
  `shaderc_glsl_task_shader` / `shaderc_glsl_mesh_shader`. Add its lavapipe hash to `screenshots.sha256` (CI computes it; this branch's hash
  predates main's sRGB swapchain change and must be regenerated).
- `src/rhi/README.md`: stages, the draws, the required features, the example.
- `ngen-example-mipchain`, `R32_SFLOAT` and per-mip storage views (`RhiDescriptorWrite::mipLevel`) were for the depth pyramid: occlusion only.

## 3. The GPU scene

### Scene types

- `src/scene/mesh.h`: `MeshletDesc` (as `PackMeshlet`), `MeshDesc` gains `meshlets`, `meshletVertices`, `meshletTriangles`; `SubMesh` gains
  `meshletOffset`, `meshletCount`, and `bounds` (section 5).
- `ViewScene::mesh` copies them from the pack.
- `RenderMeshInstance` gains `meshletOffset`, `meshletCount` (the submesh's, mesh-relative); `placement.cpp` fills them per submesh instance.

### Pools

- Pools, each a `GrowableBuffer` with its own `PoolAllocator`: `vertices` (interleaved `Vertex`, 11 floats), `positions` (3 floats per
  vertex, same index), `meshlets` (`GpuMeshlet`), `meshletVertices` (`uint32_t`), `meshletTriangles` (`uint32_t`). The index pool goes. The
  vertex and position pools gain `Storage` usage (the mesh shaders read them). `GpuScene::poolReady()` is true once all five exist; nothing
  binds the geometry set before that.
- `GpuMeshlet` (32 bytes), packed at upload; offsets rebased to pool positions so a shader reads a meshlet without its mesh's ranges:

```cpp
struct GpuMeshlet {
    uint32_t vertexOffset = 0;   // pool position in meshletVertices: range.meshletVertexOffset + meshlet.vertexOffset
    uint32_t triangleOffset = 0; // pool position in meshletTriangles
    uint32_t counts = 0;         // vertexCount | triangleCount << 8
    uint32_t cone = 0;           // the four int8 cone bytes, byte c at bits 8c
    glm::vec3 center = glm::vec3(0.0f);
    float radius = 0.0f;
};
static_assert(sizeof(GpuMeshlet) == 32);
```

- Meshlet vertex indices stay mesh-local; the shader adds the mesh's `vertexOffset` (from the mesh table).
- `GpuMeshEntry {int32_t vertexOffset; uint32_t meshletOffset; uint32_t meshletCount; uint32_t triangleCount;}` (16 bytes). A mesh not in
  the pool has `meshletCount == 0` and is never drawn: that replaces `indexCount == 0` everywhere (cull pass, render-LOD residency, tools).
- `GpuMeshRange` gains `meshletOffset/Count`, `meshletVertexOffset/Count`, `meshletTriangleOffset/Count`, `meshletBytes`; drops the index range.
- The geometry pool's stats become per part: `PoolUse {limit, capacity, used}` for vertices, meshlets, meshlet vertices, meshlet triangles.
  On main those stats are `RenderDebugGeometryPool` in `src/renderer/scenegpustats.h`; the `gpuscene` record (`scenecommands.cpp`) and
  `RenderTools::buildRenderDebugSnapshot` read them.

### The instance record

128 bytes, std430. The branch's final layout, with main's later fields:

```cpp
struct GpuInstanceRecord {
    glm::mat4 model;
    uint32_t material = 0;
    uint32_t mesh = 0;
    uint32_t meshletOffset = 0; // the submesh's meshlets, from the mesh's first
    uint32_t meshletCount = 0;
    glm::vec3 boundsMin = glm::vec3(0.0f);
    uint32_t flags = 0;
    glm::vec3 boundsMax = glm::vec3(0.0f);
    uint32_t lodLevel = 0;
    uint32_t lodGroup = 0;
    uint32_t triangleCount = 0; // the submesh's, for counts
    uint32_t streamRoot = 0;    // main's
    uint32_t lodDepth = 0;      // main's
};
static_assert(sizeof(GpuInstanceRecord) == 128);
```

- `indexOffset/indexCount` become `meshletOffset/meshletCount`; a pad word becomes `triangleCount`; `primFirst` and `primHasCutout` flags go
  (section 5). That leaves exactly enough for main's `streamRoot` and `lodDepth`: the record stays 128 bytes.
- Every GLSL copy of `Instance` must match: `meshlet.glsl`, `instancecull.comp`, `gbuffercutout.frag`. `gpuschema.cpp` describes it for the
  capture decoder; add `triangleCount` and the meshlet fields, and a schema for `GpuMeshlet` that decodes `counts` (vertices, triangles) and
  `cone` (axis, cutoff, or "none") as fields of their own (as `Bits` it showed only the low byte).

## 4. Culling into meshlet work: one draw per region

### The first design and why it was replaced

The first version kept the cull/scan/scatter and wrote one task command per visible instance (`{ceil(meshlets / 32), 1, 1, instance}`,
stride 16, `drawMeshTasksIndirectCount` per region, instance at `gl_DrawID`). Measured in release at the test world's `dense_forest`: the GPU
frame went from 30.0 ms (indexed) to 92.6 ms, almost all in the double-sided cutout regions: 93,467 camera draws and 182,265 in cascade 2,
about 23 triangles each (grass, foliage), at about 0.25 µs a mesh-task draw against 0.05 µs an indexed draw. **Per-draw cost of mesh-task
draws is about 5× an indexed draw: never issue a draw per small instance.**

### The design that stayed

- **Regions**: view × bucket, `region = view * 4 + bucket`; bucket bit 0 double-sided, bit 1 cutout. Views: 0 camera, 1–4 shadow cascades
  (occlusion added view 5, the camera's late phase; without occlusion there are 5 views, 20 regions).
- **Cull** (mode 0, one thread per instance slot): frustum per view, render-LOD level test, visibility bits; per workgroup counters per
  region: draws, primitives (`inst.triangleCount`) and **meshlets** (`inst.meshletCount`).
- **Scan** (mode 1, one workgroup): per counter, walks the workgroups in order and writes each workgroup's start offset, for draw records
  and for work items (`groupOffsets` holds `regionCount` draw offsets then `regionCount` work-item offsets per workgroup). Then thread 0 lays
  the regions out one after another in the frame slot's work-item buffer and writes each region's `RegionWork`:

```cpp
struct RegionWork {
    RhiDrawMeshTasksIndirectCommand tasks; // ceil(items / 32) task workgroups, x capped at maxTaskGroupsX = 32768, the rest in y
    uint32_t firstItem = 0;
    uint32_t itemCount = 0;
    uint32_t pad[3] = {};
};
static_assert(sizeof(RegionWork) == 32);

struct MeshletWorkItem {
    uint32_t instance = 0;
    uint32_t meshlet = 0; // the meshlet's pool position: mesh.meshletOffset + inst.meshletOffset + k
};
```

  Every device takes 65,535 groups a dimension; the y wrap is needed at the test world's counts. Work items past the buffer's capacity are
  counted (`counterWorkOverflow`), not drawn.
- **Scatter** (mode 2, one thread per slot): per region, a workgroup prefix of membership and of meshlet counts; a member writes its
  `DrawRecord {instance, meshletCount, firstItem, pad}` at its region's offset + its place, and its meshlets as work items
  `{m, meshletBase + k}` at the region's `firstItem` + its workgroup's offset + the meshlets before it. Instance order is kept, so output is
  deterministic.
- **Draw records** stay, one per drawn instance per region, for the tools (draw log, render debugger, GPU Scene window's Draws tab); the
  regions are packed one after another, each as large as the instances the CPU counted in its bucket (`DrawLists::countInstance`); a record
  that would pass its region's end is counted (`counterRegionOverflow`), not written. That counter must stay 0; it shows when the CPU's bucket
  rule and the shader's disagree. Main's LOD fades put every fading render-LOD instance in its cutout bucket, so the CPU counts every
  instance with a LOD group in its cutout bucket too (`b < 2` → also `b + 2`).
- **One draw per region**: `cmd->drawMeshTasksIndirect(regionWork, region * sizeof(RegionWork), 1, sizeof(RegionWork))`. A task workgroup
  takes 32 work items, whatever their instance.
- **Consequence**: the material's texture index now differs within a draw. Fragment shaders index `textures[nonuniformEXT(slot)]`
  (`gbuffer.frag`, `gbuffercutout.frag`, `cutout.glsl`) with `GL_EXT_nonuniform_qualifier`, and the device must have
  `shaderSampledImageArrayNonUniformIndexing`. The renderer README's old rule "indexed with one value per draw" goes.
- Baselines were byte-identical between the per-instance and per-region designs.

### The work-item buffer

- One buffer per frame slot (`cull.workitems`, 8 bytes an item). Sized from the live meshlets: `ensureWorkCapacity(liveMeshlets * 2)` when
  instances change (the camera and the cascades each see a part), grown when a readback reports overflow (trace event `MeshletWorkGrown`
  with the overflow and new capacity). Minimum 64 Ki items, doubling, capped at the device's largest buffer.
- **Bug found and fixed**: frames already in flight when the buffer grew report the same overflow, each doubling again (Sponza went
  131,072 → 4,194,304 items, 32 MiB a slot, for about 180 k used). Each slot records the capacity its frame was built with; a readback from a
  frame built with less reports no overflow.
- `DrawLists` keeps `liveMeshletCount`, updated in `countInstance` (+/− the instance's `meshletCount`); `CullResult::sceneMeshlets` reads it.

## 5. Subset bounds: every view draws per submesh

A prim's mesh draws as one instance per submesh. Before meshlets, each instance carried the whole mesh's bounds, and cascades drew the prim's
first instance with the whole mesh (`primFirst`), except prims with a cutout submesh (`primHasCutout`). With work items, drawing the whole
mesh saves no draws (the same meshlets, in the same order, since a prim's submeshes are consecutive slots and consecutive meshlet ranges), so:

- `SubMesh::bounds` computed at load (`subsetBounds(mesh, submesh)` over the submesh's indices, in `src/scene/mesh.h`; `meshBounds` and
  `meshBoundsOf` go); `placement.cpp` takes each instance's world bounds from its submesh's box.
- Every view draws the instance's own submesh; `perSubmesh` and `primFirst`/`primHasCutout` leave the cull shader, `RenderMeshInstance`,
  `GpuInstance`, the record's flags (bits 0 and 4 free), `DrawLists::report`, the render debug snapshot (`primFirstInstances`), the GPU Scene
  window's flag text and the Culling window's text.
- `DrawLists::bucketOf(instance)` is the same in every view: double-sidedness and cutout.
- Sponza: camera instances culled whole 161 → 192 of 407; cascade 0 culls 180 subset instances (35 prims before). Triangles drawn barely
  moved: meshlet culling already removed most of it. Shadow maps byte-identical.
- No pack format change for bounds: the view derives them in one pass, as it derived the mesh's.

## 6. The shaders

### `shaders/meshlet.glsl`: shared by the task shader and every mesh shader

- The geometry set (one per frame slot, bound by every mesh pass). The branch's final bindings; without occlusion drop 13 and renumber:

| Binding | What |
|---|---|
| 0 | UBO: `view`, `proj` (the branch added `cullViewProj` for occlusion; not needed without it) |
| 1 | `sampler2D textures[1024]` (fragment) |
| 2 | instances |
| 3 | materials |
| 4–8 | vertices (`float[]`, 11 per vertex), positions (`float[]`, 3 per vertex), meshlets, meshlet vertices, meshlet triangles |
| 9 | mesh table |
| 10 | the slot's work items (`uvec2[]`) |
| 11 | the slot's cull params (the planes, `cameraPosition`, `cullEnabled`) |
| 12 | the slot's meshlet counts (`uint[]`, written by the task shaders) |
| 13 | the depth pyramid (occlusion only) |
| 14 | the slot's `RegionWork[]` |
| 15 | render-LOD states (main's fades and LOD tag) |

- Stage flags for those bindings: `Task | Mesh | Fragment` (fragment for 2–3, the cutout shaders read material and opacity).
- The cull params struct in `meshlet.glsl` must be a prefix of `instancecull.comp`'s (the region offsets go last for that reason).
- Helpers: `meshletDraw(slot)` reads the payload's meshlet and instance and the mesh's `vertexOffset`; `meshletVertexCount/TriangleCount`
  from `counts`; `meshletPoolVertex(meshlet, vertexOffset, local)`; `meshletTriangle(meshlet, t)` unpacks the word; `poolPosition`,
  `poolNormal`, `poolColor`, `poolTexCoord` from the float pools.
- Payload: `struct MeshletPayload { uint meshlets[32]; uint instances[32]; uint reasons[32]; }; taskPayloadSharedEXT MeshletPayload payload;`
  (384 bytes; `reasons` only for the classify debug view).
- Push constants, the first two words of every mesh pass: `MeshletPush {region, flags}`; flags `meshletFlagCount` (this pass adds to the
  meshlet counts), `meshletFlagSplit` (two mesh workgroups per meshlet, the debug views), `meshletFlagClassify` (keep every meshlet with the
  reason it would be culled). The shadow pass's push is `{region, flags, pad0, pad1, mat4 lightViewProj}` (the matrix 16-byte aligned).

### `shaders/meshlet.task` (32 threads, one work item each)

- `item = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 32 + lid` (the y wrap); out of range → not visible.
- Sphere: `center = (inst.model * vec4(meshlet.center, 1)).xyz`, `radius = meshlet.radius * scale`, `scale` the largest column length of the
  model's 3×3.
- Frustum: the sphere against the region's view's six normalized planes (from `Frustum::fromViewProj` on the CPU; the camera's far plane
  always passes, the projection being reversed-Z with no far plane).
- Normal cone (camera only, single-sided buckets only, only with `cullEnabled`, only under uniform scale): decode the four sign-extended
  bytes / 127; cutoff ≥ 1 means no cone; `axis = model * cone.xyz / scale`, flipped when `determinant(model) < 0` (mirroring turns
  geometric normals over); back-facing when `dot(toCenter, normalize(axis)) >= cone.w * length(toCenter) + radius` with
  `toCenter = center - params.cameraPosition`. Uniform scale test: `max - min` axis length ≤ `max * 1e-3`; otherwise skip (conservative).
  Double-sided surfaces show their backs, so no cone there. Cascades: sphere only.
- `params.cameraPosition` is derived on the CPU from the culling view-projection (the point it maps to clip `(0, 0, 1, 0)`), so frustum and
  cone always follow the culling camera: with main's debug camera rendering while the main camera culls, meshlet culling is the main
  camera's with no extra work (only occlusion needed more; see "Occlusion, left out").
- Counts: shared counters (tested, frustum, cone, drawn, triangles; the branch also had occluded) summed with shared atomics, then thread 0
  adds the non-zero ones to `meshletStats[view * countersPerView + c]` when `meshletFlagCount`. Atomic sums: the totals don't depend on order.
- **Ordered compaction**: each thread writes its survive bit to shared memory; a survivor's place is the number of survivors before it; it
  writes the payload there. Not atomics (order would depend on scheduling) and not subgroup ballots (lavapipe's subgroups are narrower than
  32). Then `EmitMeshTasksEXT(total, 1, 1)` (`total * 2` with `meshletFlagSplit`).

### The mesh shaders (64 threads, one meshlet)

- `layout(triangles, max_vertices = 64, max_primitives = 124) out;` `SetMeshOutputsEXT(vertexCount, triangleCount);` thread `v` writes vertex
  `v` if `v < vertexCount`; triangles in a loop `for (uint t = v; t < triangleCount; t += 64u)` into `gl_PrimitiveTriangleIndicesEXT[t]`.
- `out gl_MeshPerVertexEXT { invariant vec4 gl_Position; } gl_MeshVerticesEXT[];` in `gbuffer.mesh` and `depth.mesh` with the identical
  expression `ubo.proj * ubo.view * model * vec4(poolPosition(vertex), 1.0)`, so the geometry pass's `Equal` test after the depth prepass
  holds.
- Per-vertex outputs are arrays (`layout(location = 0) out vec3 fragNormal[];`); flat outputs work as with vertex shaders. Fragment shaders
  keep their inputs unchanged.
- `gbuffer.mesh` (normal, colour, uv, texture slot, instance + 1 for picking, main's LOD output `fragLod` uvec2: tag + depth bits and fade
  coverage), `depth.mesh` (positions only), `depthcutout.mesh` and `shadowcutout.mesh` (uv, material, fade coverage for `cutout.frag`),
  `shadow.mesh` (light view-projection from the push constant). Normalize the normal in the mesh shader (the model carries scene unit scale).
- Per-instance values (LOD tag, fade coverage, texture slot) are computed once before the vertex branch and written to every vertex.
- **Main's render-LOD fades must be ported** to the mesh shaders: main's vertex stages read `lodStates` at set 0 binding 4, which collides with
  the vertex pool. The branch put it at binding 15 and `#include "lodfade.glsl"` in `meshlet.glsl`; `lodCoverage(inst.lodGroup,
  inst.lodLevel)` in the cutout mesh shaders, `lodTag(...) + min(inst.lodDepth, 15) * lodTagDepth` in `gbuffer.mesh`. The cull shader puts a
  fading group's instances in the cutout buckets (`bucketOf(inst, fading)`), in cull, scatter and, with occlusion, the late phase.
- **Main's streaming view**: `debugview.mesh` writes `gIds` as `uvec4(instance, mesh, material, inst.streamRoot)`.
- Deleted: `gbuffer.vert`, `depthonly.vert`, `depthcutout.vert`, `shadow.vert`, `shadowcutout.vert`, `debugview.vert`, `debugview.geom`. On a
  rebase, main's later edits to those files are what needs porting (they were the LOD fade, LOD tag and stream root).

### The debug views' mesh shader

- The old geometry shader computed triangle size, edge distances (wireframe) and primitive index; a mesh shader sees whole triangles anyway.
  For edge distances every triangle needs vertices of its own, so `debugview.mesh` handles half a meshlet: the task shader launches two mesh
  workgroups per meshlet (`meshletFlagSplit`), each up to 62 triangles and 186 vertices (`max_vertices = 186, max_primitives = 62`, under the
  256 limit). Same position expression as `gbuffer.mesh`, `GreaterOrEqual` against the frame's depth.
- Primitive id: `meshletInMesh * 124 + triangle` (`DebugView::PrimitiveId`'s comment says so).
- `debugview meshlet`: a hashed colour per meshlet (meshlet index in its mesh, and instance). `debugview meshletcull`: the task shader keeps
  every meshlet with its reason (kept, frustum, cone; the branch also had depth), drawn into a depth buffer of its own because culled
  meshlets aren't in the frame's depth. Frustum and cone results only show from elsewhere: freeze the culling frustum (or use the debug
  camera) and move away. Debug view numbering after rebasing onto main: streaming 9, meshlet 10, meshletcull 11 (`debugview.h`,
  `debugview.frag`, `debugview.comp` must agree).

## 7. The passes

- Every mesh pass (geometry, depth prepass, shadow, debug view) uses the geometry set; the shadow pass no longer has its own set or
  `bindInstanceBuffer`. Pipelines: `GraphicsPipelineRequest{.task = "shaders/meshlet.task", .mesh = "shaders/<pass>.mesh", .fragment = ...}`,
  push constant stages `Task | Mesh` (+ `Fragment` for the debug view).
- A pass declares: `builder.read(drawHandles.regionWork, IndirectRead | StorageRead)`, `builder.read(drawHandles.workItems, StorageRead)`,
  `builder.read(instanceBuffer, StorageRead)`, `builder.write(drawHandles.meshletStats, StorageWrite)` if it counts, `lodStates` if valid.
- Per region and bucket: bind the bucket's pipeline and the set, push `{region, flags}`, `PROFILE_GPU_ZONE(cmd, regionName(region))`,
  `drawRegion`, `lists.report(ctx, region, instances)` (the draw log). Skip while `!scene.poolReady()` or the set is null.
- Which passes count: the camera's geometry pass and the shadow pass (`meshletFlagCount`). The prepass draws the same meshlets as the geometry
  pass and doesn't count. The meshlet counts buffer is zeroed by the cull scan each frame and read back with the culling readback.
- Pass `primitives` stay the culled instances' triangles (an upper bound); the meshlet counts say what was drawn.

## 8. Counts, records and windows

- **The step order, everywhere**: scene → after instance culling (frustum, render LOD) → drawn (after meshlet culling). The headline
  "drawn" triangles (Culling window, status bar, `culling` record's `triangles`) are what the task shaders kept; the instance-level number
  stays as `instanceTriangles`.
- `CullResult`: `cameraDrawn`, `cameraInstanceTriangles`, `cascadeInstanceTriangles`, `cameraMeshlets` and `cascadeMeshlets`
  (`MeshletCounts {tested, frustumCulled, coneCulled, drawn, triangles}`), `sceneMeshlets`, `regionDraws`, `regionMeshlets`,
  `regionTriangles`; `cameraTriangles` and `cascadeTriangles` become the meshlet-drawn triangles.
- `culling` record (`displaycommands.cpp` on main): per view `meshlets`, `instanceTriangles`, `triangles`; `regions[]` with draws, meshlets,
  instance triangles (non-empty regions only); `sceneMeshlets`. `gpuscene` record: the pool parts. `memory`: the pool buffers
  (`gpuscene.pool.meshlets`, `...meshletvertices`, `...meshlettriangles`) and `cull.workitems`. `render`: meshes with meshlet counts and
  bytes; draws with their meshlets.
- Culling window: takes `const CullResult&` (the view's `latestCull`, alive for the loop; on main the UI gets it through `UiHooks::afterCamera`
  → `ViewUI::setCullResult`) instead of copied fields; a step table; a "Colour meshlets by culling" switch that sets the `meshletcull` debug
  view (on main: `renderconfig::debugView`).
- GPU Scene window and `gpuscenecapture` (rendertools on main): instance rows show meshlet offset/count and the mesh's meshlet count; the
  Draws tab reads `DrawRecord`s at each region's offset from the captured `cullParams` (`regionOffsets`, `commandsUsed`), showing "meshlets
  from work item"; capture `InstanceCull cullParams` for it.
- Debug View window: legend and readout for `meshlet` and `meshletcull`.
- The Engine Map had "Meshlet build" and "Meshlet cull" nodes; main removed the Engine Map, so where meshlets sit in the flow belongs in the
  renderer README instead.

## 9. Performance: what was measured and what it means

All release builds, AMD Radeon 890M (RADV), the test world (`assets/streamtest-world/world/root.usda`, about 3 M instance slots). These are
the first measurements, before the performance work; the final numbers are in section 9b.

| `flight_10` | Frame GPU | Triangles drawn | Geometry pass |
|---|---|---|---|
| main (indexed) | 49 ms (user measured 52) | 13.98 M | 21.6 ms |
| meshlets, no occlusion | **44 ms** | 13.6 M | ~7 ms (+ prepass when on) |
| meshlets, occlusion on | 67 ms | 4.77 M | 6.9 ms + 5.8 prepass |

Pass times at `flight_10`, meshlets with occlusion (ms): InstanceCull 8.5, Scan 3.0, Scatter 13.9, Shadow 6.3, Prepass 5.8, Geometry 6.9;
main: InstanceCull 8.3, Scan 2.8, Scatter 13.7, Shadow 5.0, Geometry 21.6.

What it says:
- **Meshlets win the draw**: the geometry pass falls about 3×. Meshlets alone beat main.
- **The shadow pass got a little slower** (5.0 → 6.3 ms): cascade instances are mostly one tiny meshlet each (about 19 triangles), so the
  per-meshlet overhead (a task lane, a 64-thread mesh workgroup for 20 triangles) dominates. Average fill at `flight_10`: 248 k meshlets for
  4.77 M triangles, about 19 a meshlet, against a capacity of 124. Foliage meshes are the cause.
- **The cull compute pass dominates both builds** (about 25 ms for cull, scan, scatter over 3 M slots, 2 M of them frustum-culled). Each pass
  reads the 128-byte instance record for every slot (about 380 MB a pass), on an integrated GPU's shared memory bandwidth. The scatter's
  per-region workgroup scan (8 Hillis–Steele steps, 2 barriers each, per region) is the other cost. This is main's cost too.
- **Measuring**: `record render` turns on the draw log, which makes `CullReadback` copy every draw record (about 7 ms in both builds). Read
  frame times from the `profile` record's `frames[].gpuMs`, not the render record's pass sum; the pass sum overlaps and exceeds the frame.
- Release vs debug matters: the debug shader rule packs shaders unoptimized. Compare release builds; the asset server and view must be the
  same variant, started in the same directory.

Ideas at this point, not yet built (section 9b says which were built later and what they gave: the subgroup scan, the slim record and the
packs were; chunk-level culling wasn't):
- **Subgroup operations** (core in Vulkan 1.1) in the cull pass: `subgroupAdd` before one shared atomic per subgroup for the counters, and
  `subgroupExclusiveAdd` with one shared step instead of the Hillis–Steele loop in the scatter. Ordered results stay ordered (prefix sums,
  not atomics). The task shader's ordered compaction avoided subgroups because of lavapipe's narrow subgroups; a compute workgroup can handle
  any subgroup size by combining per-subgroup totals in shared memory.
- **A slim culling record** (bounds, flags, mesh, LOD group and level: 32–48 bytes) beside the 128-byte draw record, read by every cull pass:
  about 3–4× less bandwidth for the 25 ms that both builds pay.
- **Small instances**: many instances of one tiny meshlet each make the meshlet path pay per-instance and per-meshlet overhead for 20
  triangles. Options to measure: merge tiny foliage meshes into larger clusters at authoring/pack time (authored, not computed: an authoring
  fix first), or a mesh-shader path that handles several small meshlets per workgroup.
- **Chunk-level culling** (reject authored streaming chunks before their instances) would remove most of the 3 M slot visits for any build;
  it is closer to roadmap streaming work than to meshlets.

## 9b. Performance after the squeeze (plan_meshlet_performance.md)

Measured later on this branch: release, AMD Radeon 890M (RADV), the test world, median frame GPU time over 60 frames at frame 300 (the
`profile` record), occlusion on. Each step was kept only if it won, and stacks on the one above:

| Step | `flight_10` | `dense_forest` | `overlook` |
|---|---|---|---|
| Starting point (two-phase occlusion in the depth prepass) | 68.9 ms | 55.5 ms | 62.8 ms |
| 1. Skip empty regions and slots not drawn | 36.4 | 23.3 | 45.9 |
| 2. Parallel scan | 31.0 | 18.6 | 42.8 |
| 3. A box per 256 slots | 27.7 | 13.8 | 41.5 |
| 4. Occlusion in the geometry pass | 22.7 | 13.8 | 29.6 |
| 5. Packed meshlets | 19.9 | 14.1 | 25.9 |
| 6. Slim culling record | **15.3** | **12.2** | **19.2** |
| main (indexed, no occlusion) | 49 | – | – |

Drawn at the end: `flight_10` 4.95 M triangles (4.77 M before step 4, whose first phase isn't tested against the pyramid), `dense_forest`
0.40 M (0.11 M before step 4), `overlook` 18.81 M (18.75 M); drawn instances unchanged by every step, meshlets and triangles changed only by
step 4. Lit images and shadow maps at `flight_10` and
`dense_forest` are byte-identical from step 4 on. The culling compute (cull, scan, layout, scatter, occlusion, late phase) fell from about
50 ms at `dense_forest` to about 3 ms; what is left is drawing, at about 1.3 to 1.4 billion triangles a second.

In order of what mattered, the lessons for a reimplementation:

1. **Culling 3 M slots is bound by memory.** Give the culling passes their own compact record (48 bytes: bounds, flags, LOD, mesh, meshlet
   range, triangles) instead of the 128-byte draw record; the GPU fetches whole 128-byte lines, so reading "only the fields you need" of the
   draw record still costs the whole record. This alone was 4 to 7 ms.
2. **Never let a pass do work for regions or slots it has nothing in.** The scatter's per-region workgroup scans ran for every region in every
   workgroup; reading the workgroup's own counters first and skipping empty regions (and unread slots) halved the frame.
3. **Parallelise the scan across counters** (one workgroup per counter, subgroup prefix sums), not one thread per counter walking 11,600
   workgroups: 2.9 ms → 0.1 ms, twice a frame.
4. **A box per workgroup of slots**, kept as slots change, tested before the slots: a group outside every view skips its 256 reads.
5. **Don't draw twice for occlusion.** Two-phase occlusion in a depth prepass followed by an Equal-test geometry pass cost more than it saved
   once culling was cheap; draw the first phase straight into the G-buffer, build the pyramid from its depth, draw the second phase onto it.
6. **Pack small meshlets** (several per mesh workgroup, in order) when content has many ~20-triangle meshlets. The task payload lives in memory
   on AMD: carry what mesh lanes need (pool offsets, starts) in it once, read it at uniform addresses for one-meshlet packs and via shared
   memory for multi-meshlet ones; a naive packed version was slower than no packing.
7. **What didn't pay** on this GPU: per-triangle culling in the mesh shader (exact with a half-pixel margin, but slower), 32-thread mesh
   workgroups, subgroup tricks in the task shader, larger task workgroups, 128-vertex packs, cone culling in shadow cascades (not exact).
8. **Measure with frame times, not pass times**: pass timestamps overlap (a readback showed 7 ms and cost 0.15 ms). Check every step for
   byte-identical images (lit and `--view=shadowmap`) and identical `culling` counts; a shader restructure alone can flip a pixel through
   floating-point contraction, so compare the change against its own "off" switch built the same way.
9. **After all that, drawing runs at about 1.3 to 1.4 billion triangles a second** whether writing the G-buffer or depth only: primitive
   throughput bounds it, and only fewer triangles (LOD) move it.

## 10. Determinism and baselines

- Mesh shading orders primitives by draw, task workgroup, mesh workgroup, primitive index (Vulkan "Primitive Order"). Work items are in
  instance order, the task shader compacts in order: a frame's draw order is fixed, runs are byte-identical.
- Against the indexed path, depth ties resolve in another order: Sponza differed in 2 or 3 pixels of 3.7 M, by at most 3 levels;
  three_cubes byte-identical. Re-take baselines in the step that changes them, with each difference explained.
- Sponza's checks still hold with meshlets: 29 textures (26 at 4096², 3 at 1×1), `cutoutMaterials` 1, `cutoutInstances` 4.

## 11. Pitfalls, in the order they bit

1. Unwelded meshes: meshlets a sixth full. Weld before building.
2. Mesh-task draws per small instance: about 0.25 µs each; 275 k draws a frame cost 60 ms. Draw per region over work items.
3. Non-uniform texture index once a draw spans instances: `nonuniformEXT` and the 1.2 feature, or garbage textures on some drivers.
4. `maintenance4` for SPIR-V 1.6 `LocalSizeId`; `--target-env=vulkan1.3` for `.task`/`.mesh`; GLSL 460 for `gl_DrawID` in a task shader.
5. A buffer read as indirect arguments and as storage: a combined state, or the frame graph drops the indirect stage.
6. Pipeline statistics queries with input assembly/vertex/clipping counters active during mesh shading: validation errors.
7. Work-item buffer growth from stale overflow readbacks of frames already in flight.
8. Cone culling under non-uniform scale and mirrored models; cone on double-sided surfaces; cone in shadow views.
9. Whole-mesh bounds on subset instances: subsets can't be culled apart. Subset bounds, and every view per subset.
10. The wireframe/triangle-size debug view needs unshared vertices: split meshlets into halves to stay under 256 output vertices.
11. Instance record words: keep every GLSL copy and `gpuschema.cpp` in step; main's `streamRoot` and `lodDepth` use the last two words.
12. Binding collisions with main's features (render-LOD states were at binding 4); give every geometry-set binding a fixed table.
13. A descriptor set that binds an image the frame leaves in another layout fails validation even if that path doesn't read it (hit by the
    depth pyramid's second source; the general lesson holds for any per-source set).

## 12. Occlusion, left out

The branch's occlusion culling is out of scope here. To build meshlets without it, drop: the depth pyramid (`depthpyramid.*`, `hzbbuild.comp`,
`hzb.glsl`, `mipchain` example, `R32_SFLOAT`, per-mip storage views), view 5 and its regions (20 regions, not 24), `bitEarly`/`bitLate`, the
history buffer, mode 3 (`occlusion()`) and mode 4 (`clearHistory()`), the `phase` push word and the scan/scatter's phase split,
`meshletFlagOcclusion`, `meshletFlagHiddenOnly`, `meshletFlagCullCamera`, the `occluded` counters and reason, `cullViewProj` in the UBO and the
culling-camera depth, the `render.culling.occlusion` config value and `occlusion` verb, `DepthPrepassLate`, `HzbBuild(Final)`, and the
`occlusion` block of the `culling` record. The depth prepass stays an option of its own (`render.prepass`), and the geometry pass keeps its
`Equal`-after-prepass pipelines. What occlusion taught that matters to a later attempt: as built it visited every slot three more times
(occlusion test, late scan, late scatter: 29 ms at `flight_10`) for little gain where drawing is already cheap; compact the camera's
frustum-visible instances in the first scatter and run the second phase over that list only. Section 9b's work made occlusion pay without
that list: once the late passes skip empty regions, read visibility before the instance, and use the slim record, and occlusion runs in the
geometry pass rather than a prepass, the full frame with occlusion is 15.3 ms at `flight_10`.

## 13. Visibility checklist for the reimplementation

- Records: `culling` (meshlets per view, regions, instance vs drawn triangles, `sceneMeshlets`), `gpuscene` (pool parts), `memory` (pool and
  work-item buffers), `render` (meshes' meshlets, draws' meshlets).
- Verbs: `debugview meshlet`, `debugview meshletcull` (through main's config: `render.view.debug`).
- Windows: Culling (step table, meshlet rows, meshletcull switch), GPU Scene (meshlet fields, draw records), Debug View (legends), Memory.
- Trace: `MeshletWorkGrown` (once per real overflow); `PipelineCreateFailed` covers a broken mesh shader.
- Tools: `ngen-packdump` meshlet counts per mesh.
- The renderer README: scene data on the GPU, culling and draw lists (regions, work items, records), passes, device requirements, known gaps.
