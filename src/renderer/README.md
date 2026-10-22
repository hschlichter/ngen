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
- **Deterministic output.** The same scene and camera produce the same pixels, run to run. Screenshots leave the UI out: the frame that takes one
  skips the editor UI pass, unless `setScreenshotsShowUi(true)` (ngen-view's `--show-ui`). Screenshots are compared byte for byte, so anything that
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
| Resource lifetime and upload | `deletionqueue.h/.cpp`, `gpuuploader.h/.cpp` |
| Pipelines | `pipelineregistry.h/.cpp` (`PipelineRegistry`): every pipeline and shader module |
| Shadows | `shadowcascades.h/.cpp` |
| Editor support | `imguibackend.h/.cpp` (interface), `gizmo.h`, `axis3dgizmo`, `translategizmo`, `rotategizmo`, `scalegizmo` |
| Debugging and introspection | `renderdebug.h`, `renderdebugjson.h/.cpp`, `screenshot.h/.cpp`, `capture.h/.cpp` (`CaptureService`), `gpuschema.h/.cpp`, `framedebug.h/.cpp`, `gpucounters.h/.cpp`, `debugview.h` |

Shaders live in `shaders/` at the repository root. They are packed assets, streamed from the asset server (`src/asset/README.md`) and turned into
pipelines by the pipeline registry ("Pipelines", below).

## Pipelines

`PipelineRegistry` creates every pipeline the passes draw with, and nothing else creates one.

- **A pass registers its pipelines in `init()`, as requests**: the RHI pipeline description with shader ids (`ShaderRef { id, stage }`) in
  place of modules, and owned arrays, so the registry can create the pipeline at any time. It gets a stable `PipelineHandle` and asks
  `registry.get(handle)` while the frame graph is built or recorded. Debug names are the registered names (`geometry.pipeline.cullback.less`).
- **Nothing waits for a shader.**
  - **At start or first use.** At-start pipelines (`PipelineUse::AtStart`) have their shaders requested at start-up, in one request
    (`Renderer::requestShaders`). First-use pipelines (`PipelineUse::FirstUse`, the debug views) request theirs the first time they are asked
    for (`get`, or `ready` for a group).
  - **Pending until made.** Either kind is pending, and `get` returns null, until `update()` has made it.
  - **Passes are left out until ready.** Each pass says when its pipelines are ready (`ready()`), and the frame leaves it out until then. Until
    the lit path is ready (culling, shadow, geometry, the prepass when on, lighting, AA), the frame is the backbuffer cleared to the background
    colour (`ClearBackbuffer`), the overlays whose pipelines are ready, and the UI. The scene uploads go on meanwhile.
  - **A failed pipeline doesn't stop the view.** When a shader fails to pack, its pipeline is failed and its pass stays out. The reason is in the
    trace and the pipeline's info, `--fail-on-validation` makes the run exit with 2, and a fixed source retries it (hot reload, below).
- **One point in the frame.** `update()` runs on the render thread before the frame graph is built: it creates pending pipelines whose shaders
  have arrived (polling `AssetClient::find`, so the render thread never waits on the network), at most four per frame. Nothing changes a
  pipeline during a frame's recording; debug builds assert it.
- **Shader modules are shared.** One module per shader id, for every pipeline that uses it (`shaders/shadow.frag`: the shadow pass and the
  depth prepass).
- **Observable.** Events `ShadersRequested`, `ShaderLoaded` (id, version), `PipelinePending`, `PipelineCreated`, `PipelineCreateFailed`
  (reason), `ShaderPackFailed`; the `pipelines` record (name, `atStart` or `firstUse`, state, shaders and versions, the last error) and the Render
  Debug window's Pipelines tab.
- **Hot reload.** `reload(ids)` (the renderer's `reloadShaders`, with the ids the asset server said changed) requests the shaders it has
  modules for again. When the request is answered, `update()` creates the new module and recreates every ready pipeline using it; the handle
  keeps its index, and the old pipeline and module go to the deletion queue, since frames in flight may use them. A failed pipeline using the
  shader, including one whose shader never made a module, is tried again. A shader that fails to pack or to become a module keeps the old module and pipelines, with the error in the trace
  (`ShaderReloadFailed`) and the pipelines' info until a later version loads or the source goes back. Events `ShadersReloadRequested`,
  `ShaderReloaded` (version, pipelines recreated), `PipelineRecreateFailed`.
- **Not yet**: shader reflection, creation off the render thread, a disk pipeline cache.

## How the pieces fit

```
main thread                                   render thread (RenderThread)                       GPU
-----------                                   ----------------------------                       ---
ViewScene -> RenderWorld  --RenderUpload-->   Renderer::uploadRenderWorld
                                                GpuScene: geometry pool, mesh/material tables,
                                                          instance records (delta)
                                                DrawLists / InstanceCullPass descriptor sets
camera, view UI           --RenderSnapshot--> Renderer::render
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
- **`SceneChanges`** (main to render, on scene changes): what changed, with the added meshes' and textures' data shared, applied at the start of the
  next render frame. Lists submitted between two frames are appended and applied together, in order.
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

1. Wait for the slot's fence. Drain the deletion queue up to that slot's last frame, read its GPU zones (profiler lane) and its culling
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
  Passes without side effects whose outputs nobody reads are culled (`culled` in the `render` record's passes). Transient texture lifetimes are computed over the order.
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

`GpuScene` owns the scene's GPU tables, and changes them piece by piece. The view hands the renderer `SceneChanges` (`renderworld.h`): meshes,
textures and materials added and removed, draw slots changed, the lights. `Renderer::applySceneChanges` applies them in a fixed order:
- **Removals first**, so what is added reuses their pool ranges, table entries and texture slots. A whole-scene repack then doesn't grow the pool.
- **Then textures, meshes and materials**, before the draws that name them.
- **Then the draw slots.**

Applying only queues uploads. Nothing waits.

Tables:

- **Geometry pool.** One interleaved `Vertex` buffer, one position-only buffer (12 bytes per vertex, for depth-only passes) and one 32-bit index buffer.
  - **A range per mesh**, from a first-fit allocator (`PoolAllocator`) that merges neighbouring free ranges. A mesh is one mesh asset, shared by every
    prim that places it.
  - **Growth.** When a range doesn't fit, a buffer twice the size replaces the old one. The old contents are copied on the GPU before this frame's
    uploads, and the old buffer is retired.
  - **Limit.** No pool buffer grows past the device's largest buffer (`RhiDeviceLimits::maxBufferBytes`, whole MiBs), or a smaller limit
    (`setGeometryLimit`, ngen-view's `--geometry-limit=<MiB>`). The vertex stream reaches it first and sets the vertex capacity of both vertex
    buffers. A mesh that doesn't fit waits, with an empty mesh table entry, so its draws draw nothing while the rest of the scene draws; the
    first one traces `GeometryPoolFull` (a warning). Once a frame after removals freed ranges, the waiting meshes that fit are placed, in
    arrival order (`GeometryPoolFits` when none is left). The `memory` record's `geometryPool` has the limit, capacities, use and what waits.
  - **Addressing.** Indices stay mesh-local; a mesh is addressed by `firstIndex` and `vertexOffset`, and the vertex and position pools share
    `vertexOffset`.
  - **No compaction.**
- **Mesh table.** `GpuMeshEntry` per `MeshHandle::index`: `firstIndex`, `vertexOffset`, `indexCount`. Handles are never reused. A mesh whose data
  hasn't been copied yet, or was removed, has `indexCount` 0 and isn't drawn. The table grows like the pool, and the part past the old contents is
  zeroed.
- **Textures and slots.**
  - **One GPU texture per `TextureHandle`**: a texture asset, or a generated 1×1 of a constant colour, shared by the materials naming it.
  - **Created late.** It is created when its first level is copied, so creating large textures follows the upload budget.
  - **A slot once complete.** It takes a slot in the texture array once every level is copied, at the start of the next frame (`beginFrame`). Slot 0
    is the fallback texture and slot 1 a white 1×1, which a material samples while its texture uploads. Freed slots are reused, up to
    `GpuScene::maxTextures` (1024).
- **Material table.** A `GpuMaterial` entry per `MaterialHandle`; freed entries are reused.
  - **Contents**: the base colour texture slot and, for a cutout material, the opacity: its texture slot (the base colour's when the opacity is that
    texture's channel), the channel and raw flag (`opacityInfo`), and the threshold.
  - **Rewritten when a texture it names takes its slot.** Until then it samples the white texture, and a cutout whose opacity texture is still
    uploading draws opaque.
- **Instance buffer.** One `GpuInstanceRecord` (128 bytes, std430) per draw slot.
  - **Contents**: model matrix, material entry, mesh index, submesh `indexOffset` and `indexCount`, world bounds, flags (`primFirst`,
    `doubleSided`, `boundsValid`, `cutout`, `primHasCutout`), the drawn LOD level + 1 of the LOD root above it (0 under none), which the
    geometry pass writes to the normal target's alpha for the LOD level view, and its render-LOD group + 1 (0 for none; "Render LOD").
  - **Slots.** A draw keeps its slot while it exists. A removed one is dead (mesh index past the table, so culling never draws it) until a new draw
    takes it.
  - **Reading it.** Vertex shaders read it at `gl_InstanceIndex`, and every draw sets `firstInstance` to the slot.
  - **Uploads.** Only the changed records go up: packed in the slot's staging buffer (grown up to 4 MiB; a larger upload, as a tile switch's or
    after the buffer grew, takes a staging buffer of its own for its frame), and copied by the `InstanceUpload` pass in one copy with a region per
    run of consecutive slots (`RhiCommandBuffer::copyBufferRegions`). The `gpuscene` record has the last upload's records and regions. The buffer
    grows by recreation.

**Uploads** (`GpuScene::addUploadPass`, the `SceneUpload` pass, before the culling passes):
- **Per frame.** Each frame takes queued uploads in order, up to `uploadBudget` bytes (64 MiB; at least one upload, so a large texture level still
  goes). A texture's levels are separate uploads.
- **Seen from outside.** After every frame the render thread publishes `SceneGpuStats` (`RenderThread::latestSceneGpuStats`): change lists
  submitted and drawn, the tables' sizes, the upload queue (`GpuScene::uploadStats`: queued, budget, the last pass, textures waiting for their
  slot) and the geometry pool. The budget and the pool limit are runtime settings (`RenderThread::setUploadBudget`, `setGeometryLimit`), applied
  before the next frame.
- **One staging buffer per frame's batch**, retired through the deletion queue.
- **Barriers.** The copies are recorded in the frame's command buffer:
  - before them, a barrier from the tables' and pool's read states to transfer;
  - one between a grown buffer's move and the uploads, since copies in one command buffer may run in any order;
  - one back after.

  On one queue a barrier also orders against earlier submissions, so ranges freed this frame are written at once.
- **Start-up.** `Renderer::flushSceneUploads` records everything on `GpuUploader`'s command buffer and waits, so the first frame has the scene.
- **Descriptor sets.** When the texture slots change or a table or buffer is replaced, the descriptor sets naming them are rewritten at the next frame
  (`Renderer::rebuildGeometryDescriptorSets`, which also rebuilds the culling sets; the old sets are retired).

`GpuInstance` is the CPU mirror the renderer keeps (`Renderer::gpuInstances`), by draw slot.

## Culling and draw lists

`DrawLists` defines the command layout and owns the per-slot culling buffers; `InstanceCullPass` runs `shaders/instancecull.comp` in three modes.

- **Regions.** One region per (view, bucket), four buckets per view (`DrawLists::bucket`): single-sided (back-face culling), double-sided, and the
  same two for cutout materials. View 0 is the camera, views 1..4 the shadow cascades. Camera regions hold every visible instance with a mesh in the
  pool, drawing its submesh range. Cascade regions hold `primFirst` instances (one per prim), drawing the whole mesh, except for a prim with a cutout
  submesh (`primHasCutout`): its instances draw one by one, each its submesh range, so the cutout one goes to a cutout bucket and is cut there too.
- **Region sizes.** An instance's bucket follows from its flags, so it can land in one camera region and one region per cascade. The renderer
  keeps the live instances per bucket as slots change (`DrawLists::countInstance`, the shader's bucket rule on the CPU), and each frame
  `writeParams` packs the regions one after another, each as large as its bucket's count (`CullParams::regionOffsets`): the command buffer holds
  the camera's instances plus four cascades' at most, not every instance in every region. The scatter never writes past a region's end; a
  command it drops there is counted (`counterRegionOverflow`, the `culling` record's `regionOverflow`) and traced (`CullRegionOverflow`, a
  warning, once per change): the CPU's bucket rule and the shader's disagree, and those draws are missing. It is 0 otherwise. Each instance's world bounds are tested against six normalized planes per view (`Frustum::fromViewProj`, computed on the CPU into the
  slot's parameter buffer) with the positive-corner test. Instances without valid bounds are always visible, and with culling off every instance is. A
  frozen frustum is only a different camera matrix in the snapshot (`CullState`).
- **Ordered compaction (reduce-then-scan).** `InstanceCull` counts, per workgroup of 256 instances, how many commands each region gets.
  `InstanceCullScan` turns the per-workgroup counts into start offsets and totals. `InstanceCullScatter` writes each command at its workgroup's offset
  plus its position in a shared-memory prefix sum. Commands stay in instance order, so draw order and depth-tie resolution match run to run. Atomic
  appends would be simpler but make the order depend on scheduling. The scan loops over workgroups on one thread per counter, so it stays cheap into the
  millions of instances.
- **Counts and statistics.** The totals buffer holds the draw count per region (read by `drawIndexedIndirectCount`), primitives per region,
  camera-culled instances, and culled and drawn `primFirst` instances per cascade.
- **Per-slot buffers.** Parameters, visibility, counters, offsets, commands and counts are per frame slot and imported every frame. The
  per-instance ones grow with the instance count, the command buffer with the instances counted in the buckets. The graph has no
  transient buffers yet: a pooled buffer reused next frame would need synchronisation against the previous frame's reads.
- **Readback.** `CullReadback` copies the totals and the visibility bits into the slot's host-visible readback buffer, and while the render debugger
  is open the commands into the slot's commands readback, made when it is asked for and let go the first frame the slot's frame isn't; `DrawLists::parseReadback` reads them after the slot's
  fence, each region at the offset that frame laid it out at. `CullResult` (the view's `culling` record, Culling window, AABB
  overlay), the per-pass `draws`/`primitives` (through `addIndirectStats`) and the draw log all come from there.

The mesh passes bind the pipeline, descriptor set and pool once per bucket and issue one `drawIndexedIndirectCount` at the region's offset with
its size as `maxDrawCount`. An empty region still costs one call; only the GPU knows the count.

## Render LOD

A render-LOD root (a UsdLod root whose levels aren't payloads; `src/world/README.md` "Loading") where the view places it is a *group*
(`RenderLodGroup`, slots in `SceneChanges::lodGroups` beside the draw slots): the bounding sphere of its heuristic's extent in world metres,
its thresholds (up to 8 levels), its default and a forced level (hand-set, or the default while the heuristic is off). Every level's draws are
slots tagged with the group (`lodGroup`) and their level (`lodLevel`). Each frame the GPU picks the level a group draws, and culling draws only
that level's slots. `LodGroups` (`lodgroups.*`) holds the buffers; `LodSelectPass` (`passes/lodselectpass.*`, `shaders/lodselect.comp`) runs
before the culling passes.
- **The table** (`gpuscene.lodgroups`, `GpuLodGroup`, 64 bytes): uploaded as groups change (`LodGroupUpload`, the changed span through the
  slot's staging, as the instance records). A slot taking a new group gets a new serial.
- **The pick** (`LodSelect`, one thread per group): the forced level, else the level the view's screen-size heuristic wants (the sphere over the
  view window at depth 1 from the live camera, finer as soon as the size passes a threshold, coarser once 20% below it, against the level the
  group wanted last frame); then the level drawn: the wanted one when its meshes are in the pool, else the one drawn last frame when they are,
  else the nearest resident level, coarser first. Written to the state buffer (`gpuscene.lodstate`: the serial, the level wanted and drawn).
- **Residency** (`gpuscene.lodresidency`, a bit per level): the cull pass, which visits every alive slot, sets its level's bit when its mesh
  isn't in the pool (its mesh table entry is empty: not copied yet, or waiting for room); the next frame's pick reads and clears it.
- **Persistence.** The table, the state and the residency mask persist across frames, imported with the access the last frame left them in. A
  grown one is new and its contents undefined: the state is written by the pick before culling reads it, and stale residency bits only make a
  group draw another resident level for a frame.
- **Counts** (`LodSelectSum`, per-workgroup counters summed in order, as the culling scan): groups alive, drawing each level, falling back, at a
  forced level; read back with the culling readback (`LodReadback`), with every group's state when asked (`RenderSnapshot::readLodStates`)
  and the camera the pick measured from, into `CullResult`. The cull pass counts the slots of levels not drawn (`counterLodDropped`), in no view and not as culled.

## Cutout materials

A material with `opacityThreshold > 0` (`MaterialDesc::cutout`) draws only where its opacity reaches the threshold; there is no blending. Its
instances go to the cutout buckets, whose pipelines `discard` below the threshold (`shaders/cutout.glsl`, `cutAway`): the geometry pass through
`gbuffercutout.frag`, the depth prepass and the shadow pass through `cutout.frag` with `depthcutout.vert` and `shadowcutout.vert`, which read the full
vertex stream for the texture coordinate instead of the position pool and bind the geometry set. The opaque buckets keep shaders without `discard`,
so early depth testing holds for everything else. The debug views discard in `debugview.frag` itself, for every bucket.

- **The opacity** is one channel of a texture, or a constant. A constant at or above the threshold is opaque; below it, the surface is cut away
  entirely (opacity texture slot 0).
- **Raw masks.** Textures are uploaded sRGB, so a mask authored `raw` is decoded as colour when sampled; the shader re-encodes the sampled value
  (linear to sRGB, not for alpha) before the comparison, which gives back the stored value to 8-bit precision.
- **Shadows** are cut as the camera's view is: a prim with any cutout submesh casts per submesh.
- **Picking** reads `gbuffer.instance`, so it goes through what is cut away.

## Passes

| Pass | What it does |
|---|---|
| `InstanceUploadPass` | Copies the dirty span of instance records from the slot's staging buffer into the instance buffer. Added only when something changed. |
| `LodSelectPass` | `LodSelect` and `LodSelectSum`, the render-LOD pick above; left out when there are no groups. `LodGroups` adds `LodGroupUpload` when groups changed and `LodReadback`. |
| `InstanceCullPass` | The three culling passes above; the cull pass also drops the slots of render-LOD levels not drawn and marks levels not resident. |
| `ShadowPass` | Depth-only into the cascade atlas: per cascade, the tile's viewport and scissor, the cascade matrix as a push constant, one indirect call per bucket. Position pool only, except the cutout buckets. |
| `DepthPrepass` | Optional depth-only pass over the camera regions so the geometry pass can use an Equal depth test and shade each pixel once. `depthonly.vert` mirrors `gbuffer.vert`'s position expression with `invariant gl_Position`. |
| `GeometryPass` | G-buffer: albedo (RGBA8, alpha carries the sampled mip level for the mip view), normals (RGBA16F), the instance id (`gbuffer.instance`, R32_UINT: instance index + 1, 0 where nothing is drawn), depth. Eight pipelines: opaque or cutout, cull back or none, Greater-with-write or Equal-without after the prepass. |
| `DebugViewPass`, `DebugViewResolve` | Only while a debug view is on (`DebugView`): the camera regions drawn again through `debugview.vert/.geom/.frag` into an RGBA32F value target (`debugview.value`), then coloured by `debugview.comp` into `debugview.color`, which the blit takes instead of the AA result. |
| `LightingPass` | Fullscreen deferred shading into a float scene-colour target: one directional light (the first shadow-enabled directional light, else the first directional), cascade selection by view depth, hardware-compare PCF, shadow tint, a hemisphere ambient (an engine sky colour on faces turned up and ground colour on faces turned down, as fractions of the light's radiance), and the debug views (`GBufferView`) and overlays. |
| `AAPass` | FXAA as a compute pass, scene colour to a float storage image; falls back to a blit when the format cannot be a storage image. |
| `BlitToBackbuffer` | Blits the AA result (or the debug view colours) to the sRGB backbuffer. |
| `DebugLinePass` (`DebugRenderer`) | Debug lines from `DebugDrawData` (AABBs, frustum outlines, light gizmos), depth-tested against the scene depth. |
| `GizmoPass` | Wide-line gizmo geometry: the axis gizmo the renderer owns and the translate, rotate and scale gizmo vertices the main thread computes. |
| `EditorUIPass` | Renders the cloned ImGui draw data through `ImGuiBackend`. |
| `PresentPass` | Moves the backbuffer to `PresentSrc`; no GPU work. |

**Picking** reads the id target: a pick asks the capture service for the one texel of `gbuffer.instance` under a pixel, right after
`GeometryPass`, and gets the instance index back a few frames later (ngen-view's `ViewPicking`, `src/view/README.md`). Nothing is read
back without a pick, and the target is never shown, so screenshots don't change.

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
- **`GpuUploader`.** One command buffer, a staging buffer per upload, one submit and a blocking fence wait in `end()`. Start-up only: the fallback
  textures, and the scene's first upload (`flushSceneUploads`). The scene's uploads after that go through `GpuScene`'s per-frame queue.
- **`ResourcePool`.** Transient textures for the frame graph, reused across frames by size, format and usage; flushed on swapchain recreation.
- **Textures.** Material textures are RGBA8 sRGB with full mip chains. Textures from files arrive packed by the asset server with every level
  (`src/asset/README.md`), and are uploaded as they are; the generated 1×1 textures of constant colours have one level. The material sampler is
  trilinear with anisotropy and editable live (`SamplerSettings`, `sampler` session verb); changing it rebuilds the geometry descriptor sets.

## Shadows

Cascaded shadow maps in one atlas: one tile for a single cascade, a 2×2 grid otherwise (three cascades by default, `ShadowCascadeSettings`). The main
thread fits the cascades (`fitShadowCascades`: practical split scheme, bounding-sphere ortho frusta snapped to texels) from the near plane to the
shadow distance (`ShadowCascadeSettings::maxDistance`, 200 m by default, the Render Debug window's slider and `shadow distance=`), pulled in to the
scene bounds; beyond it surfaces are lit without shadow. It puts them in the snapshot; the GPU culling tests instances against each cascade; the
lighting pass picks the cascade by view depth and samples the atlas with a hardware compare sampler (3×3 PCF by default). The shadow maps use
standard depth (cleared to 1, compare `LessOrEqual`): each cascade's orthographic depth is linear and fitted to it, so reversing it gains nothing.

## Depth

The camera uses reversed-Z with no far plane (`cameraProjection` in `src/camera.h`): depth is `near / distance`, 1 at the near plane (0.1 m) and
falling to 0 at infinity, so nothing is clipped at a distance and the 32-bit float depth keeps its precision far away.
- **Conventions.** Camera depth is cleared to 0. The depth prepass and geometry pass write with `Greater`, the geometry pass tests `Equal` after
  the prepass, the debug views test `GreaterOrEqual` and the debug lines `Greater`. `RhiDepthState`'s default (`Less`) is the shadow pass's.
- **Reading it.** `lighting.frag`: background where depth is 0, view distance `near / depth` (cascade selection), world position through the inverse
  view-projection. The depth preview is a log scale from the near plane to the scene's farthest corner.
- **Frustum and rays.** `Frustum::fromViewProj(viewProj, reversedDepth)` keeps the plane order (4 near, 5 far) for the camera and the cascades; the
  camera's far plane has no normal and always passes. `Frustum::corners` takes the depths of its two planes, so the frozen-frustum overlay ends
  at the shadow distance. `Ray::fromCamera` unprojects depths 1 and 0.5 for the gizmos' mouse rays.

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
  `memoryHeaps`). `renderdebugjson` writes it for the view's `render` record and the memory part for its `memory` record.
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
  named by its region (`DrawLists::regionName`). The zones of a slot are read after its fence and handed to the profiler's GPU lane on the CPU clock. While counters are enabled (`setCountersEnabled`), the frame graph also wraps each pass in a pipeline statistics query, and
  `GpuCounters` (zones plus statistics per pass) is queued per frame for the Counters window and the view's `counters` record.
- **Culling introspection.** `instancecull.comp` writes, next to the visibility bits, the frustum plane that rejected each instance in each view
  (`cullPlanes`, 4 bits per view: plane 0–5, `D` a render-LOD level its group doesn't draw, `E` visible, `F` not tested). The readback keeps every view's visibility bit (`CullResult::viewBits`).
- **Trace events** (Render category, conventions in `src/trace/README.md`). The renderer traces flow and problems, never per-frame data:
  `ShaderLoaded` per startup shader, `FirstFramePresented` once (ms since the process started), `SceneUploaded` when an upload changes geometry (instances, meshes, textures, ms; a transform edit is silent), `SwapchainRecreate` and
  `SwapchainRecreateFailed`, `Screenshot` and `TextureDump` per file written (`ScreenshotFailed`, `TextureDumpFailed`), `ShaderLoadFailed`,
  warnings for fallbacks (`FormatFallback`, `DebugViewsOff`, `TextureSlotsFull`), and the RHI's messages (`RhiMessage`: validation and native
  errors, through `RhiDeviceOptions::onMessage`). Per-frame numbers are in the view's records: `render` (passes with draw counters and GPU time,
  culled passes, meshes, textures, frame totals, draw log), `counters` (GPU zones and pipeline statistics), `culling`, `memory` (every buffer and
  texture, the geometry pool and instance buffer among them) and `profile`.
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
