# Renderer

`src/renderer/` turns the scene's render data (`RenderWorld`) and a per-frame `RenderSnapshot` into GPU work. It runs on a dedicated render thread,
builds a frame graph every frame, draws the scene with GPU-driven indirect draws, and talks to the GPU only through the RHI (`src/rhi/README.md`). This
file describes how the renderer is structured, how its components fit together, and the rules the code relies on. Read it before adding a pass or
changing how scene data reaches the GPU.

## Principles

- **GPU-driven is the only path.** Scene data lives in persistent GPU tables; visibility and draw lists are produced on the GPU; the mesh passes issue one
  indirect draw per pipeline bucket per view. There is no CPU-driven fallback to keep in sync.
- **The frame graph owns ordering and barriers.** A pass declares what it reads and writes; the graph orders passes, culls unused ones, allocates transient
  textures and inserts every texture and buffer barrier. Passes do not record barriers for graph resources.
- **Deterministic output.** The same scene and camera produce the same pixels, run to run. Screenshots are compared byte for byte, so anything that
  reorders draws (for example atomic appends) is a behaviour change, not an implementation detail.
- **The render thread owns the GPU.** The main thread never records or submits. It hands over values (snapshots, uploads) and receives values back
  (debug snapshots, culling results).
- **What the CPU shows about the GPU is read back.** Culling results, draw counts, the draw log and GPU timings arrive after the frame slot's fence, one
  frame-slot cycle late.

## Layout

| Area | Files |
|---|---|
| Orchestration | `renderer.h/.cpp` (`Renderer`), `renderthread.h/.cpp` (`RenderThread`), `rendersnapshot.h`, `renderworld.h`, `renderertypes.h` |
| Frame graph | `framegraph.h/.cpp`, `framegraphbuilder.h`, `framegraphcontext.h`, `framegraphresource.h`, `passnode.h`, `framegraphdraw.h`, `framegraphdebug.h`, `framegraphpreviews.h/.cpp`, `resourcepool.h/.cpp` |
| GPU scene and culling | `gpuscene.h/.cpp` (`GpuScene`), `drawlists.h/.cpp` (`DrawLists`), `passes/instanceuploadpass`, `passes/instancecullpass`, `culling.h/.cpp` (`CullState`) |
| Passes | `passes/`: shadow, depth prepass, geometry, debug views, lighting, AA, blit, debug lines, gizmos, editor UI, present |
| Resource lifetime and upload | `deletionqueue.h/.cpp`, `gpuuploader.h/.cpp`, `mipchain.h/.cpp`, `shaderloader.h/.cpp` |
| Shadows | `shadowcascades.h/.cpp` |
| Editor support | `imguibackend.h/.cpp` (interface), `gizmo.h`, `axis3dgizmo`, `translategizmo`, `rotategizmo`, `scalegizmo` |
| Debugging and introspection | `renderdebug.h`, `renderdebugjson.h/.cpp`, `screenshot.h/.cpp`, `capture.h/.cpp` (`CaptureService`), `gpuschema.h/.cpp`, `framedebug.h/.cpp`, `gpucounters.h/.cpp`, `debugview.h` |

Shaders live in `shaders/` at the repository root and are compiled to SPIR-V next to the executable by the build.

## How the pieces fit

```
main thread                                   render thread (RenderThread)                       GPU
-----------                                   ----------------------------                       ---
USD scene -> RenderWorld  --RenderUpload-->   Renderer::uploadRenderWorld
                                                GpuScene: geometry pool, mesh/material tables,
                                                          instance records (delta)
                                                DrawLists / InstanceCullPass descriptor sets
camera, editor, gizmos    --RenderSnapshot--> Renderer::render
  (cull frustum, cascades,                      wait slot fence, read back last use of the slot
   ImGui, debug lines)                          build FrameGraph (passes declare accesses)
                                                compile (order, cull, lifetimes)
                                                execute into the slot's command buffer  ------>  cull, draw, light, post
                                                submit, present
editor windows            <--latest-only----  FrameGraphDebugSnapshot, RenderDebugSnapshot,
                                              CullResult
```

### Threads and handoff

- **`RenderSnapshot`** (main to render, every frame): view and projection, world up, window size and mouse, view mode and overlay toggles, AA and depth
  prepass switches, material sampler settings, the culling frustum (`cullViewProj`, live or frozen) and `cullEnabled`, shadow settings and the fitted
  cascades, gizmo vertices, debug line data and the cloned ImGui draw data. One slot with back-pressure: the main thread blocks if the render thread has
  not taken the previous snapshot.
- **`RenderUpload`** (main to render, on scene changes): a `RenderWorld` copy plus shared mesh and material libraries, applied at the start of the next
  render frame.
- **Back to the main thread**, each latest-only and non-blocking: `FrameGraphDebugSnapshot` (when the Frame Graph window is open),
  `RenderDebugSnapshot` (when the Render Debug window is open or a dump is requested) and `CullResult` (every new readback).
- **Small requests** go straight to the `Renderer` or `RenderThread` under a lock: texture inspect, texture dump, screenshot.

### Renderer

`Renderer` owns the device-facing state: the swapchain, the depth texture, the per-slot UBOs and sync objects, `GpuScene`, `DrawLists`, the passes,
`FrameGraph`, `ResourcePool`, `DeletionQueue`, `GpuUploader` and the texture cache. Its entry points:

- `init`: swapchain, samplers, fallback texture, every pass's pipelines, `GpuScene` and `DrawLists` (the instance buffer exists from here on), frame
  sync, the ImGui backend and frame-graph previews.
- `uploadRenderWorld`: rebuilds `GpuInstance`s from the `RenderWorld`, updates instance records (transform changes are a delta), and on any other change
  rebuilds the geometry pool, textures, material table and descriptor sets.
- `render`: one frame (below).
- `destroy`: waits idle, drains the deletion queue, destroys in reverse.

**Frame slots.** The renderer keeps one slot per swapchain image. A slot owns its command buffer, in-flight fence, image-available semaphore, view UBO,
instance staging buffer, culling buffers and readback buffers; the slot's fence guards all of them. The render-finished semaphore belongs to the swapchain
image, because present consumes it per image.

**A frame (`Renderer::render`).**

1. Wait for the slot's fence. Drain the deletion queue up to that slot's last frame, read its GPU zones (`GpuTime`, profiler lane) and its culling
   readback.
2. Acquire a swapchain image; on `OutOfDate` recreate the swapchain and depth texture and skip the frame.
3. Write the slot's view UBO. Pick the shadow light and the cascades (from the snapshot, or one fitted here if the snapshot has none).
4. Build the frame graph: import backbuffer and depth, add the instance upload, the culling passes, the draw passes, the readback, lighting, AA,
   overlays, UI and present.
5. Compile, then execute into the slot's command buffer. Screenshot and texture-dump copies are recorded after the graph.
6. Submit with the image-available and render-finished semaphores and the slot's fence, present, advance the slot.

## The frame graph

- **`FrameGraph`** holds the passes and resources of one frame. `reset()` clears it, `importTexture`/`importBuffer` add external resources,
  `addPass<Data>(name, setup, execute)` adds a pass, `compile()` orders and culls, `execute(cmd)` records.
- **`FrameGraphBuilder`** is what a pass's setup sees: `createTexture`, `read`/`write` for texture and buffer handles with an `FgAccessFlags` value
  (`ColorAttachment`, `DepthAttachment`, `ShaderRead`, `StorageRead`, `StorageWrite`, `TransferSrc`, `TransferDst`, `IndirectRead`, `Present`), and
  `setSideEffects`.
- **`FrameGraphContext`** is what a pass's execute sees: the command buffer, `texture(handle)`/`buffer(handle)` for the physical resources, `logDraw`
  for the draw log and `addIndirectStats` for draws the RHI cannot count.
- **Compile.** Edges run from each resource's writer to its readers and between consecutive writers; a topological sort gives the execution order.
  Passes without side effects whose outputs nobody reads are culled (`PassCulled`). Transient texture lifetimes are computed over the order.
- **Execute.** Per pass: allocate transient textures that start here from `ResourcePool`, derive and record the barriers for every declared access, run
  the pass inside a debug label, a GPU zone and a CPU profile zone, record its `RhiCommandStats`, then release transients that end here.
- **Textures.** Created textures are transient and pooled by size, format and usage. Imported textures start every frame undefined; that is right for
  the backbuffer and for a depth buffer that is cleared each frame.
- **Buffers.** Imported only. `importBuffer(..., initialAccess)` takes the access the previous frame left the buffer in and `finalAccess(handle)` returns
  it after `execute`, so a persistent buffer synchronises across frames; the caller keeps that state next to the buffer. Per-slot buffers import with no
  prior access, since the slot's fence already ordered them. Read-after-read of the same kind needs no barrier; anything involving a write does.
- **Stats.** A pass's stats include the barriers the graph issued for it.
- **Debugging.** `buildDebugSnapshot()` feeds the Frame Graph window (pass list, resources, node view). With the window open, `FrameGraphPreviews`
  captures a thumbnail of each colour texture at its last use through the debug capture hook.

## Scene data on the GPU

`GpuScene` owns the scene's GPU tables. Two upload paths, chosen by the data:

- **Bulk** (large, rare, blocking through `GpuUploader`): the geometry pool, the mesh table and the material table, rebuilt whenever `uploadRenderWorld`
  sees a change other than transforms (mesh, material, submesh range, instance count).
- **Delta** (small, per frame, through the graph): instance records. `uploadRenderWorld` tracks the lowest and highest changed instance; the next frame
  writes that span into the slot's staging buffer and `InstanceUpload` copies it into the persistent instance buffer.

Tables:

- **Geometry pool.** One interleaved `Vertex` buffer, one position-only buffer (12 bytes per vertex, for depth-only passes) and one 32-bit index buffer
  for every mesh. Indices stay mesh-local; a mesh is addressed by `firstIndex` and `vertexOffset`, and the vertex and position pools share `vertexOffset`.
- **Mesh table.** `GpuMeshEntry` per `MeshHandle::index`: `firstIndex`, `vertexOffset`, `indexCount`. A mesh outside the pool has `indexCount` 0.
- **Material table and texture slots.** Slot 0 is the fallback texture; each textured material gets the next slot, in instance order, up to
  `GpuScene::maxTextures` (1024). `GpuMaterial` holds the base colour texture slot. Untextured materials carry their colour as a 1×1 texture.
- **Instance buffer.** One `GpuInstanceRecord` (112 bytes, std430) per `GpuInstance`: model matrix, material entry, mesh index, submesh `indexOffset`
  and `indexCount`, world bounds, and flags (`primFirst`, `doubleSided`, `boundsValid`). Vertex shaders read it at `gl_InstanceIndex`; every draw sets
  `firstInstance` to the instance index. The buffer grows by recreation; the old one goes through the deletion queue.

`GpuInstance` is the CPU mirror the renderer keeps (`Renderer::gpuInstances`): one per `RenderMeshInstance`, a prim's mesh expanded per material
submesh. When a table or buffer is replaced, every descriptor set naming it is rewritten (`Renderer::rebuildGeometryDescriptorSets`, which also rebuilds
the culling sets and reports `GeometryDescriptorsRebuilt` with the reason).

## Culling and draw lists

`DrawLists` defines the command layout and owns the per-slot culling buffers; `InstanceCullPass` runs `shaders/instancecull.comp` in three modes.

- **Regions.** One region per (view, bucket). View 0 is the camera, views 1..4 the shadow cascades; bucket 0 is single-sided (back-face culling),
  bucket 1 double-sided. Camera regions hold every visible instance with a mesh in the pool, drawing its submesh range. Cascade regions hold only
  `primFirst` instances (one per prim), drawing the whole mesh: shadows are material-agnostic.
- **Visibility.** Each instance's world bounds are tested against six normalized planes per view (`Frustum::fromViewProj`, computed on the CPU into the
  slot's parameter buffer) with the positive-corner test. Instances without valid bounds are always visible, and with culling off every instance is. A
  frozen frustum is only a different camera matrix in the snapshot (`CullState`).
- **Ordered compaction (reduce-then-scan).** `InstanceCull` counts, per workgroup of 256 instances, how many commands each region gets.
  `InstanceCullScan` turns the per-workgroup counts into start offsets and totals. `InstanceCullScatter` writes each command at its workgroup's offset
  plus its position in a shared-memory prefix sum. Commands stay in instance order, so draw order and depth-tie resolution match run to run. Atomic
  appends would be simpler but make the order depend on scheduling. The scan loops over workgroups on one thread per counter, so it stays cheap into the
  millions of instances.
- **Counts and statistics.** The totals buffer holds the draw count per region (read by `drawIndexedIndirectCount`), primitives per region,
  camera-culled instances, and culled and drawn `primFirst` instances per cascade.
- **Per-slot buffers.** Parameters, visibility, counters, offsets, commands and counts are per frame slot and imported every frame. The graph has no
  transient buffers yet: a pooled buffer reused next frame would need synchronisation against the previous frame's reads.
- **Readback.** `CullReadback` copies the totals, the visibility bits and, while the render debugger is open, the commands into the slot's host-visible
  readback buffer; `DrawLists::parseReadback` reads it after the slot's fence. `RenderStats.culled`/`shadow_culled`, `CullResult` (Culling window, AABB
  overlay), the per-pass `draws`/`primitives` (through `addIndirectStats`) and the draw log all come from there.

The mesh passes bind the pipeline, descriptor set and pool once per bucket and issue one `drawIndexedIndirectCount` with the region's capacity as
`maxDrawCount`. An empty region still costs one call; only the GPU knows the count.

## Passes

| Pass | What it does |
|---|---|
| `InstanceUploadPass` | Copies the dirty span of instance records from the slot's staging buffer into the instance buffer. Added only when something changed. |
| `InstanceCullPass` | The three culling passes above. |
| `ShadowPass` | Depth-only into the cascade atlas: per cascade, the tile's viewport and scissor, the cascade matrix as a push constant, one indirect call per bucket. Position pool only. |
| `DepthPrepass` | Optional depth-only pass over the camera regions so the geometry pass can use an Equal depth test and shade each pixel once. `depthonly.vert` mirrors `gbuffer.vert`'s position expression with `invariant gl_Position`. |
| `GeometryPass` | G-buffer: albedo (RGBA8, alpha carries the sampled mip level for the mip view) and normals (RGBA16F), depth. Four pipelines: cull back or none, Less-with-write or Equal-without after the prepass. |
| `DebugViewPass`, `DebugViewResolve` | Only while a debug view is on (`DebugView`): the camera regions drawn again through `debugview.vert/.geom/.frag` into an RGBA32F value target (`debugview.value`), then coloured by `debugview.comp` into `debugview.color`, which the blit takes instead of the AA result. |
| `LightingPass` | Fullscreen deferred shading into a float scene-colour target: one directional light (the first shadow-enabled directional light, else the first directional), cascade selection by view depth, hardware-compare PCF, shadow tint, and the debug views (`GBufferView`) and overlays. |
| `AAPass` | FXAA as a compute pass, scene colour to a float storage image; falls back to a blit when the format cannot be a storage image. |
| `BlitToBackbuffer` | Blits the AA result (or the debug view colours) to the sRGB backbuffer. |
| `DebugLinePass` (`DebugRenderer`) | Debug lines from `DebugDrawData` (AABBs, frustum outlines, light gizmos), depth-tested against the scene depth. |
| `GizmoPass` | Wide-line gizmo geometry: the axis gizmo the renderer owns and the translate, rotate and scale gizmo vertices the main thread computes. |
| `EditorUIPass` | Renders the cloned ImGui draw data through `ImGuiBackend`. |
| `PresentPass` | Moves the backbuffer to `PresentSrc`; no GPU work. |

Overlays and the UI draw on the backbuffer after the AA blit, so they are never filtered.

## Descriptor sets

| Set | Used by | Bindings |
|---|---|---|
| Geometry, one per frame slot | `GeometryPass`, `DepthPrepass`, `DebugViewPass` | 0 view UBO, 1 `sampler2D textures[1024]`, 2 instance buffer, 3 material table |
| Shadow, one | `ShadowPass` | 0 instance buffer; the cascade matrix is a push constant |
| Culling, one per frame slot | the three cull passes | params, instances, mesh table, visibility, counters, offsets, commands, counts |
| Lighting, AA, debug lines, gizmos | their passes | per-slot UBOs and the G-buffer, shadow and scene-colour textures |

The texture array is fixed-size and fully written (unused slots hold the fallback) and indexed with one value per draw: `gbuffer.vert` looks up the slot
through the instance's material and passes it flat to `gbuffer.frag`. That dynamically uniform indexing holds for multi-draw indirect too, where each draw
is its own invocation group.

## Resource lifetime and uploads

- **`DeletionQueue`.** The RHI destroys immediately; the renderer defers. Each entry is tagged with the frame that last used the resource and destroyed
  once that frame's slot fence has passed. Replaced buffers, textures, descriptor pools and preview textures all go through it.
- **`GpuUploader`.** One command buffer, a staging buffer per upload, one submit and a blocking fence wait in `end()`. Used for bulk uploads (geometry
  pool, tables, textures), never per frame.
- **`ResourcePool`.** Transient textures for the frame graph, reused across frames by size, format and usage; flushed on swapchain recreation.
- **Textures.** Material textures are RGBA8 sRGB with full mip chains built on the CPU (`mipchain.h`, 2×2 box filter). The material sampler is
  trilinear with anisotropy and editable live (`SamplerSettings`, `sampler` session verb); changing it rebuilds the geometry descriptor sets.

## Shadows

Cascaded shadow maps in one atlas: one tile for a single cascade, a 2×2 grid otherwise (three cascades by default, `ShadowCascadeSettings`). The main
thread fits the cascades (`fitShadowCascades`: practical split scheme, bounding-sphere ortho frusta snapped to texels, far plane pulled in to the scene
bounds) and puts them in the snapshot; the GPU culling tests instances against each cascade; the lighting pass picks the cascade by view depth and
samples the atlas with a hardware compare sampler (3×3 PCF by default).

## Editor support

- **`ImGuiBackend`** is the interface the renderer draws ImGui through; the application owns the concrete backend (`ImGuiBackendVulkan` in `src/`) and
  the event plumbing. The main thread clones the draw data into the snapshot (`ImGuiFrameSnapshot`); `EditorUIPass` renders it.
- **Gizmos.** `TranslateGizmo`, `RotateGizmo` and `ScaleGizmo` live on the main thread: they hit-test, drag and produce vertices and new transforms;
  the renderer only draws their vertices. `Axis3DGizmo` (the corner orientation gizmo) is owned and drawn by the renderer.
- **Texture inspector and dumps.** One level of one material texture is blitted into a preview texture registered with ImGui, or copied to a buffer and
  written as PNG after the fence.
- **Screenshots.** The presented image is copied into a buffer inside the frame's command buffer and written as PNG after the fence.

## Debugging and introspection

- **Render Debug snapshot** (`RenderDebugSnapshot`): device limits, swapchain and pacing, scene tables (meshes, textures, materials, culled instances),
  per-pass stats with GPU times, pool textures, the draw log, and every live allocation and memory heap (from `RhiDevice::allocations` and
  `memoryHeaps`). `renderdebugjson` writes it for `--dump-render-debug` and the memory part for `--dump-memory`.
- **Debug names.** Every buffer, texture, pipeline, set and shader gets a name at creation (`gpuscene.pool.*`, `gpuscene.instances`,
  `material.N.basecolor`, `cull.*`, `fg.<resource>` for transient textures, `frameslotN.*`, `swapchain.imageN`). They show in validation messages,
  the Memory window, command logs and RenderDoc.
- **`CaptureService`.** Captures any frame-graph resource right after any pass, or a static scene buffer at the end of the frame. The main thread
  sends `CaptureWatch`es (one-shot on a trigger change, or live every frame; textures optionally a sub-rectangle); the graph runs an
  `FgCaptureRequest` after the named pass with the resource's physical object and state; the service copies it into a readback buffer, restoring the
  state around the copy, and parses it after the slot's fence. Results carry the raw bytes, per-channel ranges and an ImGui preview.
  `gpuschema` describes every struct that crosses to the GPU (field, offset, type) so buffers decode into rows the same way in the UI and in dumps.
  The culling readback and the frame-graph previews predate it and keep their own paths.
- **Frame debug capture** (`FrameDebugCapture`, `framedebug`). For one requested frame the command buffer's command log is on. The capture holds, per
  pass: the barriers the graph issued (`FgBarrierRecord`, always kept) with the backend's derived stages, accesses and layouts
  (`RhiDevice::describeTransition`), the logged commands, and the contents of every descriptor set a pass bound (`describeDescriptorSet`).
- **GPU timings and counters.** Every pass is a GPU zone, and every indirect call in the depth prepass, geometry and shadow passes is a nested zone
  named by its region (`DrawLists::regionName`). The zones of a slot are read after its fence, reported as `GpuTime` and handed to the profiler's GPU
  lane on the CPU clock. While counters are enabled (`setCountersEnabled`), the frame graph also wraps each pass in a pipeline statistics query, and
  `GpuCounters` (zones plus statistics per pass) is queued per frame for the Counters window and `dump-counters`.
- **Culling introspection.** `instancecull.comp` writes, next to the visibility bits, the frustum plane that rejected each instance in each view
  (`cullPlanes`, 4 bits per view: plane 0–5, `E` visible, `F` not tested). The readback keeps every view's visibility bit (`CullResult::viewBits`).
- **Observation events** (Render category, conventions in `obs.md`): `RenderStats` every 60 frames (`draws`, `indirect_draws`, `draw_commands`,
  `primitives`, `instances`, `culled`, `shadow_culled`, `instance_buffer_bytes`, `instance_upload_bytes`, `geometry_pool_bytes`, `material_count`, ...),
  `GpuTime` per frame, `PipelineStats` per pass while counters are on, `CullReadback` per readback, `InstanceUpload` per upload frame (with the access
  carried in from the previous frame), `CaptureResult` per capture, and one-off events for table and buffer creation (`GeometryPoolBuilt`,
  `MaterialTableBuilt`, `InstanceBufferCreated`, `DrawListsCreated`, `GeometryDescriptorsRebuilt`), frame begin and end, pass execution and culling,
  swapchain recreation, screenshots and texture dumps.
- **RenderDoc.** The in-app API lives in `src/renderdoccapture.*`, outside the renderer; the debug names make its captures readable.

## Device requirements

A Vulkan 1.3 device, and from the RHI beyond the basics: `shaderSampledImageArrayDynamicIndexing`, `multiDrawIndirect`, `drawIndirectFirstInstance`
and `drawIndirectCount` (all required by the Vulkan backend's `init`), timestamps for GPU timings (optional), an RGBA16F format usable as a storage
image for the compute AA (optional, blit fallback), pipeline statistics for the counters (optional) and geometry shaders for the debug views
(optional; the views are off without them).

## Known gaps

- Only one directional light is shaded; point and spot lights in the `RenderWorld` are not.
- No per-draw GPU timing. A timing zone cannot open inside a multi-draw call; the finest GPU time is per region (one indirect call).
- The draw log and per-pass draw counts are read back, so they lag, and the log exists only while the render debugger is open.
- No transient buffers in the frame graph (see "Culling and draw lists").
- The texture array is fixed at 1024 slots; materials past it use the fallback texture and a warning is printed.
- Culling is per instance against frusta only: no occlusion culling, no clusters or meshlets. Building-sized meshes cull poorly.
- Geometry, textures and tables re-upload in full on any change other than transforms.
- Everything runs on one graphics queue; the cull passes are separate graph passes so a multi-queue scheduler could move them.
