# Async scene

**Status. Landed.** 2026-10-13. Phases 1 and 2; phase 3 tried and backed out. Loading responsiveness moved to Deferred (off-thread allocation).

ngen-view never waits for its scene, and starts with the absolute minimum: no scene and no shader stands between start-up and the first
frame, and the view stays responsive throughout. It opens a scene and draws its first frame at once. Layer packs, meshes, materials and textures appear as
they arrive. A LOD switch, a purpose toggle, a Repack or a change notice updates what changed, on the GPU too, without a stall or a full
re-upload. This is the roadmap's "Everything async" for the scene, and the base the streaming plans build on
([plan_streaming_lod.md](plan_streaming_lod.md), decisions 1 and 6).

## Current state

- **`ViewScene` blocks** (`src/view/viewscene.*`, `src/view/README.md`). `open` composes round by round, fetches every mesh and material, then
  every texture, each with `AssetClient::wait`, then extracts. `setLodLevel`, `setPurposeShown` and `refresh` (Repack and change notices) do
  the same on the main loop, and the window freezes until they finish.
- **Every extract starts from nothing.** `extract()` makes a new `MeshLibrary` and `MaterialLibrary` and adds a `MeshDesc` per drawn entity.
  - A mesh asset placed many times is in the library, and the geometry pool, once per placement. Caldera's default map draws 4,427 instances of
    4,397 meshes.
  - Handles mean nothing from one extract to the next. The renderer is told the libraries were replaced (`RenderUpload::librariesReplaced`) and
    uploads everything again.
- **The GPU scene is rebuilt whole** (`src/renderer/gpuscene.*`, `renderer.cpp`).
  - `rebuildGeometry` concatenates every mesh into new pool buffers.
  - The renderer uploads every texture again (cached by material, so a texture shared by two materials is uploaded twice).
  - `rebuildMaterials` reassigns every texture slot and the whole material table.
  - Each goes through `GpuUploader`, which submits and waits: 1.7 s on Sponza.
  - Only transforms are incremental: the instance buffer's dirty span through a staging copy pass.
- **Instances are a flat list in extraction order.** Picking, the draw log and `instancePath` map an instance index back to a prim through
  `RenderWorld::primToInstance`.
- **Headless runs count frames from start-up**, and every verification script and baseline assumes the scene is loaded by frame 1.
- **Related plans.** [plan_editor_split.md](plan_editor_split.md) phase A lists the incremental GPU scene (pool allocation per mesh, material and
  slot patches, instance adds and removes), bundled with deltas, the manifest and resync. [plan_incremental_gpu_upload.md](plan_incremental_gpu_upload.md)
  predates the geometry pool and the scene packs.

## Scope

**In**
- Stable identity across rebuilds: a mesh per mesh asset, a material per material asset or display colour, a texture per texture asset,
  each kept while something draws it.
- An incremental GPU scene:
  - geometry pool ranges allocated and freed per mesh;
  - texture slots allocated and freed per texture;
  - material table entries patched;
  - instances added, removed and changed in place.
- Uploads that never block: copy passes in the frame graph, staging retired through the deletion queue, within a per-frame byte budget.
- A `ViewScene` that requests and never waits: composition and extraction off the main thread, arrivals taken as they come, changes handed to
  the renderer as a list of what changed.
- What draws while things arrive: placeholders for missing materials and textures.
- Open, LOD, purposes, Repack and change notices all through that one path. The RPC methods answer when their change is drawn.
- A start-up with nothing to wait for: no start-up shaders, every pipeline requested at start-up but created when its shaders arrive, and the
  passes that need them skipped until then (decision 10).
- Headless runs stay deterministic: script frames count from when the scene is resident.

**Out**
- Residency, budgets, priority and eviction: the streaming steps ([plan_streaming_lod.md](plan_streaming_lod.md), steps 2–5). This plan
  loads everything the scene wants, as today, just without waiting for it.
- The manifest, deltas and resync of [plan_editor_split.md](plan_editor_split.md) phase A. That phase then needs only the delta path on top of
  this one (see Steps, 7).
- Incremental composition: the composer still composes the whole world each time, on the scene thread (decision 2).
- Geometry pool compaction, an async transfer queue, texture mip streaming.

## Decisions

Locked 2026-10-13, all ten as proposed. Decision 8 uses the streaming design's "resident" rather than a word of its own; decision 10 follows
from the direction that start-up waits for nothing it can do without.

1. **A scene thread owns composition and extraction.** It holds the `WorldComposer`, the arrived packs and the libraries. It takes arrivals
   and requests (LOD, purpose, refresh), composes, extracts, and publishes two things: an immutable snapshot for the main thread's queries
   (`primPath`, `drawsOf`, `bounds`, transforms) and a list of changes for the renderer.
   - Rejected: time-slicing on the main thread. Composing Caldera's default map takes 0.6 s and extracting it 0.7 s, which can't be sliced
     without making the composer resumable.
   - I lean the thread: it keeps the main loop's frame time independent of scene size.
2. **Changes are found by diffing extracts, not by incremental composition.** Each extract keys every draw by prim path and submesh, and every
   mesh, material and texture by asset id. The scene thread compares it with the previous one and emits what was added, removed or changed.
   - Rejected for now: composing and extracting incrementally. That is a large change to `WorldComposer`, and the diff gives the renderer the
     same changes.
   - Its cost is a full compose and extract per batch of arrivals, on the scene thread. A trigger in Deferred covers when that gets too slow.
3. **Identity by asset.** One mesh per mesh pack, shared by every prim that places it. One material per material asset, or per display colour
   for unbound prims, as today. One texture per texture asset, shared by the materials naming it. Each is refcounted by the draws using it and
   freed when none do.
   - This changes the geometry pool from per-placement to per-mesh, a saving on any scene that reuses meshes.
   - A material's texture becomes a reference to a shared texture, not the material's own copy.
4. **One geometry pool, suballocated, grown by copy.** The vertex, position and index buffers each get a free-list allocator (first fit,
   neighbouring free ranges merged). When an allocation doesn't fit, the pool is replaced by one twice the size and the old contents are copied
   on the GPU, with the old buffers retired through the deletion queue.
   - Rejected: several pools. Indirect draws bind one vertex and index buffer per bucket, and several pools would multiply the draw calls.
   - Compaction stays out. The free list may fragment over a long session, which the Memory window shows.
5. **Uploads don't wait, and stay within a budget per frame.** What phase 1 needs is three things:
   - **The CPU never waits for the GPU.** Today `GpuUploader::end` submits and blocks. Instead, mesh data, texture levels, material entries and
     instance records are copied in a copy pass in the frame graph, recorded into the frame's own command buffer, as the instance upload already
     is.
   - **Staging lives until the GPU has read it.** Each upload gets a staging buffer of its own. The buffer is handed to the `DeletionQueue` at the
     frame that copies from it, as retired pool buffers and pipelines already are.
   - **A cap per frame.** At most `uploadBudget` bytes are copied a frame (default 64 MiB), and what doesn't fit waits for the next frame. A
     texture larger than the budget is split by mip level. Without the cap, opening Sponza would put about 2.5 GB into staging and copies in one
     frame: a memory spike and one long frame.

   `GpuUploader` keeps its blocking mode for start-up (the fallback texture) and gains this non-blocking one. Nothing blocks after start-up.
   - Rejected for now: a staging ring, a fixed staging block per frame slot reused every frame. It only saves allocations: the RHI has no
     suballocator, so each staging buffer is a device allocation of its own. That's nothing for Sponza, and probably fine for Caldera's 4,400
     meshes spread over several frames. It's in Deferred with a trigger.
   - Rejected: an async transfer queue. The RHI has one queue (`src/rhi/README.md`).
6. **Draw what has arrived** (streaming decision 6, applied to loading).
   - A prim draws once its mesh is on the GPU.
   - Until its material has arrived, it draws with the display colour material.
   - Until a texture has arrived, its material uses the material's constant colour (white for a textured one), through the fallback slot.
   - A cutout material whose opacity texture hasn't arrived draws opaque.
   - Nothing is held back for its siblings.
7. **Instances live in stable slots.** An instance keeps its slot in the instance buffer while it exists. A removed one is marked dead (index
   count 0, culled), and its slot is reused by the next add. Culling and the draw passes are unchanged.
   - Picking, the draw log and `instancePath` map a slot to a prim through a slot table that the scene thread keeps.
   - A fresh open fills slots in extraction order, so its draws, and its images, are today's.
   - Rejected: compacting the buffer on every change. That moves instances, so every mapping changes and every record is uploaded again.
8. **Scripts count frames from when the scene is resident.** "Resident" is the streaming design's word
   ([plan_streaming_lod.md](plan_streaming_lod.md): *requested → packing → packed → fetching → resident*, resident meaning loaded into memory
   and ready to draw). The scene is resident when everything it wants is:
   - no request unanswered;
   - the scene thread idle;
   - no upload waiting.

   Once streaming decides what is wanted, the same rule still holds.
   - `--frames`, `--screenshot` and script frame numbers count from the first frame the scene is resident.
   - A new verb `wait-resident` holds a script until the scene is resident again, for scripts that change it.
   - Every existing script and baseline keeps working unchanged.
   - `--no-wait-resident` counts from start-up, to measure loading itself.
   - The trace marks the moment: `SceneResident` (time since open, assets, uploads).
9. **RPC methods answer when their change is drawn.** `view.lod.set`, `view.purposes`, `view.repack` and `view.open` keep the responder and
   answer once the scene is resident after the change, so agents still get "done" semantics without the view blocking.
10. **No start-up shaders.** The first frame needs no shader from the asset server: the ImGui backend draws with its own embedded SPIR-V, and
    the blit to the backbuffer is an RHI blit.
    - Every pipeline is requested at start-up, in one request, and created as its shaders arrive, by the registry's `update`.
    - A pass whose pipelines aren't ready is skipped, as `DebugViewPass` already is. Until the lit path's pipelines are ready (culling,
      geometry, lighting, AA), the frame shows the clear colour and the UI.
    - `PipelineUse::Startup` and `createStartupPipelines` go away. `FirstUse` stays for pipelines requested only when first used (the debug
      views).
    - A shader that fails no longer stops the view. It is a trace error and a Pipelines-tab error, and its passes stay skipped.
      `--fail-on-validation` runs fail on it, so headless verification still catches it.
    - The resident rule (decision 8) includes pipelines: the scene isn't resident while a pipeline it draws with is pending, so scripts and
      baselines see the same frames as today.

    What start-up still waits for, and why:
    - the RHI device and swapchain;
    - the ImGui backend;
    - the connection to the asset server, because without it nothing can ever arrive;
    - one blocking upload of the 1×1 fallback texture.

    Everything else arrives on the fly.

## Steps

Two phases, each observable on its own.

### Phase 1: identity and the incremental GPU scene

The view still loads in one go, but every change after the first upload is incremental.

1. **Persistent libraries** (`src/scene/mesh.h`, `material.h`, `viewscene.*`):
   - `MeshLibrary` and `MaterialLibrary` keep their entries across extracts, keyed by asset id (and display colour). They gain `remove(handle)`
     and a generation per handle, so a reused index can be told apart.
   - Textures become entries of their own: a `TextureLibrary` keyed by texture asset id, referenced by materials as `baseColorTexture` and
     `opacityTexture` handles instead of pixel copies.
   - `extract()` produces a `RenderWorld` whose draws name these handles, keyed by `(prim path, submesh)`.
2. **The change list** (`src/renderer/renderworld.h`):

   ```cpp
   // What changed in the renderer's inputs since the last list; applied in order.
   struct SceneChanges {
       std::vector<MeshHandle> meshesAdded;
       std::vector<MeshHandle> meshesRemoved;
       std::vector<TextureHandle> texturesAdded;
       std::vector<TextureHandle> texturesRemoved;
       std::vector<MaterialHandle> materialsChanged; // added or edited
       std::vector<MaterialHandle> materialsRemoved;
       std::vector<InstanceChange> instances;        // add, remove, or update (transform, material, flags) by slot
       std::vector<RenderLight> lights;              // the whole light list, small
   };
   ```

   `ViewScene` diffs each extract against the last (decision 2) and hands the renderer `SceneChanges` instead of a whole `RenderWorld`.
   `RenderUpload::librariesReplaced` goes away.
3. **The incremental GPU scene** (`gpuscene.*`, `renderer.cpp`):
   - **Pool.** `PoolAllocator` (offset and size, first fit, merging) for the vertex, position and index buffers. `addMesh` and `removeMesh`
     replace `rebuildGeometry`, and the mesh table entry is patched. Growth works as in decision 4.
   - **Textures.** A slot allocator over `maxTextures`. `addTexture` and `removeTexture` replace the texture cache. The geometry descriptor sets
     are rewritten per slot, in the frame slot whose fence has passed.
   - **Materials.** `setMaterial` and `removeMaterial` patch `GpuMaterial` entries in place.
   - **Instances.** Stable slots with a free list (decision 7), uploaded through the existing dirty-span path, widened to a list of spans.
4. **Non-blocking uploads** (`gpuuploader.*`, decision 5).
   - `GpuUploader` gains a queue of pending uploads: a buffer range or a texture's mip levels, each with its source bytes and destination.
   - Each frame, `addUploadPass(fg, frame)` takes uploads in order until the frame's budget is spent. For each it creates a staging buffer,
     writes it, and records the copy in one copy pass before the culling passes. The staging buffer is handed to the `DeletionQueue` at that
     frame.
   - A texture in flight stays in the fallback slot until its last level has been copied (decision 6).
   - `pendingBytes()` and `pendingUploads()` feed the resident rule (decision 8) and the `render` record.
   - `begin()` and `end()` stay as the blocking path for start-up.

### Phase 2: the scene thread

5. **The scene thread** (`src/view/scenethread.*`, `viewscene.*`):
   - It owns the composer, the packs and the libraries, and takes requests from a queue: open, LOD, purposes, refresh.
   - Requests go to `AssetClient::request` without `wait`. Arrivals come from a new `AssetClient::takeArrived()` (ids answered since the last
     call), polled when the client signals.
   - Each batch is composed, extracted and diffed, then published: a `SceneSnapshot` (`std::shared_ptr<const SceneSnapshot>`: prim paths,
     slot table, bounds, `drawsOf`, `assetsUnder`, transforms) and a `SceneChanges` for the render thread.
   - The main thread's `ViewScene` keeps its interface over the latest snapshot. `setLocalTransform` stays on the main thread, as today's
     transform fast path.
6. **Residency and scripts** (`src/apps/view.cpp`, `viewcommands.*`): the resident rule, `--no-wait-resident`, the `wait-resident` verb and `SceneResident`
   (decision 8), and the RPC methods' deferred answers (decision 9).
7. **No start-up shaders** (`pipelineregistry.*`, every pass's `init`, `renderer.cpp`, `src/apps/view.cpp`), decision 10:
   - The passes register their pipelines without a use. The registry requests them all at `init`, in one request.
   - Each pass checks `registry.ready(...)` for its pipelines and isn't added to the graph until they are.
   - `view.cpp` drops the wait (`ShadersReady`) and goes straight into the main loop.
8. **The notices and the editor split.** Change notices go to the scene thread like any refresh; the blocking `refresh` is removed.
   [plan_editor_split.md](plan_editor_split.md) phase A loses its "incremental GPU scene updates" bullet, which points here, and its delta path
   lands on `SceneChanges`.
9. **Docs.**
   - `src/view/README.md`: the scene thread, when the scene is resident, placeholders.
   - `src/renderer/README.md`: the incremental GPU scene, the pool allocator, non-blocking uploads and their budget, instance slots.
   - The run-headless skill: frames counted from residency, `--no-wait-resident`, `wait-resident`.
   - [plan_incremental_gpu_upload.md](plan_incremental_gpu_upload.md) is marked superseded by this plan.

## Verification

**Phase 1**
- **A fresh open draws as today.** The lit, albedo and normals screenshots of three_cubes and Sponza are byte-identical to
  `_out/depthrange-baseline/after`.
- **Meshes are shared.** Caldera's default map reports fewer pool meshes than draws in the `render` record, where today the two match (4,397
  meshes for 4,427 instances), and less pool memory than today.
- **A LOD switch is incremental.** On `assets/layered/root.usda`, `lod /World/Tree/lod 0` uploads only the level's mesh. The trace shows one mesh
  added and no texture uploaded, and the pool's used bytes grow by that mesh. Switching back frees it.
- **A texture refresh is incremental.** Repack of one Sponza texture uploads that texture alone, rewrites one descriptor slot, and builds no
  geometry. The next frame differs from the one before only where that texture draws.
- **No upload frame is long.** During a Sponza Repack scene, the `profile` record shows no frame over 50 ms on the render thread, where today one
  frame takes about 1.7 s.

**Phase 2**
- **First frame at once, even cold.** With the asset server's cache wiped, so every shader compiles and every asset packs:
  - `FirstFramePresented` comes before the first `ShaderLoaded` and before `SceneResident` in the trace;
  - its time since start-up is the device, swapchain and ImGui set-up alone, under 300 ms;
  - the first frame shows the clear colour and the UI.
- **Warm.** Opening Sponza, `FirstFramePresented` comes before `SceneResident`, and its time since start-up is under 300 ms. Caldera's capital,
  framed, shows its first frame before its meshes have arrived.
- **Responsive throughout.** While Sponza loads cold with `--no-wait-resident`, the `profile` record shows no main-thread frame over 50 ms,
  and a `pick` or camera move scripted during loading takes effect within the next frames.
- **A broken shader doesn't stop the view.** With a syntax error in `shaders/lighting.frag` at start-up, the view runs: the trace has the error,
  the frame shows the clear colour and the UI, and fixing the shader brings the lit image without a restart (the change notice path).
- **Scripts are unchanged.** Every existing script in the run-headless skill and every baseline above gives the same records and byte-identical
  screenshots, counting frames from residency.
- **Edits apply live.** Editing three_cubes' `displayColor` while the view runs patches one material entry. The trace shows no mesh or texture
  upload, and the frames before and after differ only on that cube.
- **RPC still means done.** `view.lod.set` answers after the level is drawn, and a `pick` right after the answer hits the new level.
- **Placeholders.** With a texture's packer slowed (a script that holds its asset back), its material draws with the constant colour until the
  texture arrives, then with the texture. No validation messages.
- Debug and release build.

### Phase 1 results (2026-10-13)

All five phase 1 criteria pass.
- **A fresh open draws as today.** three_cubes and Sponza in the lit, albedo, normals and prepass views are byte-identical to
  `_out/depthrange-baseline/after`. Caldera's default view is byte-identical to the depth-range screenshot, and its camera triangles match
  (16,972,953).
- **Meshes are shared.** Caldera's default map has 4,321 meshes for 4,427 draws (4,397 before), Sponza 115 for 407 as before.
- **Start-up upload is faster.**
  - Sponza: 694 ms instead of 1,745 ms (extract 169 ms instead of 451).
  - Caldera: 602 ms instead of 2,819 ms (extract 527 ms instead of 683).
- **A LOD switch is incremental.** On `assets/layered/root.usda`, `lod /World/Tree/lod 0` adds level 0's 2 meshes and removes level 1's, with 3
  draws changed and no textures. Switching back reverses it, and the image is byte-identical to the first.
- **A texture refresh is incremental.** Repack of `wood_door_01_BaseColor.png` swaps 1 texture and its material, changes 3 draws, adds no meshes
  and queues 85 MiB. The image after is byte-identical to the one before.
- **No upload frame is long.** During `repack scene` on Sponza, no render-thread frame does more than 24.8 ms of work (0 of 198 over 50 ms):
  applying the changes takes 0.1 ms, and the uploads spread over about 126 frames at up to 15 ms each.
- **Other paths.**
  - Picking through slots selects `column_2ndfloor_12`; after `translate` moves it, the same pixel picks the wall behind.
  - A change notice for three_cubes' colour shows and reverts byte-identically.
  - The debug build runs the LOD test clean.
  - No validation messages, and debug and release build.

Changed or found on the way:
- **One staging buffer per frame's batch**, not per upload (decision 5). Caldera's open is about 17,000 uploads, four per mesh. Still no ring.
- **The upload queue lives in `GpuScene`**, which resolves its own buffers when it records, since growth can replace them. `GpuUploader` keeps
  the blocking path and gained `commandBuffer` and `adoptStaging` for `flushSceneUploads`.
- **Removals are applied first**, so a whole-scene repack reuses the freed pool ranges instead of doubling the pool. That growth took 76 ms in
  one frame.
- **Textures are created when their first level is recorded.** Creating 26 large textures at once took 98 ms in one frame.
- **A bug found and fixed.** Copies in one command buffer may run in any order. A grown table's move overwrote the first mesh entries written
  after it, and Caldera lost its 6 largest draws (2 M triangles). The fix is a barrier between the moves and the uploads, and no move from a
  buffer never written. Validation didn't catch it, since synchronisation validation isn't enabled; it's a candidate for the RHI's
  `--validation`.
- **Descriptor sets are still rewritten whole.** They're rebuilt, as before, whenever slots or buffers change, not one slot at a time.
- **The texture inspector, `inspect` and `dump-texture` take a texture index** (the `render` record's `textures` list it, with its slot and
  the materials naming it), since textures are no longer per material.
- **For phase 2: a refreshed USD file re-uploads all its meshes.** Its meshes are fetched again and get new entries even when unchanged
  (three_cubes' colour edit re-adds its 4 meshes). Phase 2's "patches one material entry" needs the entry kept when the pack's version is the
  same.

### Phase 2 results (2026-10-13)

What passes:
- **First frame at once.**
  - Warm Sponza: the first frame 37 ms after start, resident at 4.0 s.
  - Cold Sponza (a fresh server and cache in a scratch root): the first frame 37 ms after start, before any shader loaded (the shaders by
    about 350 ms), the USD file packed by 5.1 s, resident at 9.5 s.
  - Cold three_cubes: the first frame 27 ms after start in one run, but 437 ms in another, while the server compiled shaders on every core
    (contention, not a wait).
  - Caldera's capital: the first frame 33 ms after start, resident at 4.7 s, its image byte-identical to the depth-range one.
- **Scripts are unchanged.** three_cubes and Sponza in the lit, albedo, normals and prepass views are byte-identical to
  `_out/depthrange-baseline/after`. The LOD, pick and translate, and texture repack scripts give the same results as in phase 1.
- **RPC still means done.** A screenshot one script frame after `lod /World/Tree/lod 0` already shows the new level, byte-identical to the one
  30 frames later.
- **A broken shader doesn't stop the view.**
  - With a syntax error in `lighting.frag` at start-up the view runs, showing the clear colour, the trace has glslc's error, and
    `--fail-on-validation` exits with 2.
  - Fixing the file during the run retries the failed pipeline, and the final frame is byte-identical to the lit baseline.
- **Unchanged assets aren't uploaded.** A forced repack of one texture that packs to the same bytes keeps its entry: no upload.
- **Debug and release build**, and the debug build runs the LOD script and three_cubes clean.

What doesn't:
- **Responsive throughout: not met.** The longest main-thread frame while loading is 84.7 ms on warm Sponza and 435 ms on Caldera. The main
  thread waits on the render thread (one snapshot in flight), whose long frames are allocations:
  - growing a pool or table buffer, up to 68 ms (two in one change list, 90 ms);
  - a frame's staging buffer, 15–23 ms of each upload frame;
  - a 4096² texture, 12–17 ms.

  The 50 ms target needs at least the staging ring (its trigger, "staging allocations showing in the frame time", fired) and pool growth
  that doesn't create a large buffer in one frame. Both are decisions; see Open questions.
- **"Edits apply live" as written.** three_cubes' colour edit re-uploads the edited cube's mesh, not only a material entry: the display colour
  is in the mesh pack. It uploads only that mesh (1 of 4), its material and its colour texture, which is what changed. The criterion assumed
  wrongly.
- **Placeholders: not checked.** There is no way yet to hold back one asset on the server, so the white-while-uploading texture and the
  display-colour material are only seen in passing during loads.

Changed on the way:
- **`setLocalTransform` is a request to the scene thread**, not a main-thread fast path (step 5 said it stayed). The thread owns the
  transforms, so a drag waits for the thread's current batch, at most a frame or two at Sponza's size.
- **The open doesn't fail the view.** A root layer that can't be had is a `SceneFailed` error and an empty scene, and the view runs on.
- **`view.repack` answers `{assets}`.** `sceneAssets` went, since what is the scene's is known only on the scene thread.
- **The trace has `AssetsRefreshRequested`** in place of `AssetsRefreshed`, and `SceneResident` carries the longest frame while loading.
- **A failed shader's pipeline is retried by a change notice**, even with no module made.
- **Two crashes fixed:** recording an upload before any mesh existed (no pool buffers yet), and exiting before one existed.

## Responsiveness while loading

Locked 2026-10-13:
- **Phase 3 is (1) the staging ring and (2) pool sizing up front**, as recommended below. Tried and backed out the same day: neither
  moved the longest loading frame (Phase 3 results), so they were reverted as complexity without a gain.
- **A test-only delay flag on the asset server**, so the placeholder rules can be checked.
- **The plan lands when Sponza meets the target**, with Caldera recorded as a follow-up if it still misses (off-thread allocation, (3)
  below, to Deferred with its measured number).

**What costs the time.** The main thread can't run more than one snapshot ahead of the render thread, so a long render-thread frame is a long
main-thread frame. Measured on warm Sponza (`SceneResident`'s longest frame: 84.7 ms) and Caldera's capital (435 ms), the long render-thread
frames are all allocations:

| What | Where | Cost |
|---|---|---|
| Growing a pool or table buffer | `GpuScene::ensureCapacity`, from `addMesh` while applying a change list | up to 68 ms each; 90 ms with two in one list; Caldera's pool reaches about 2.3 GB |
| A frame's staging buffer | `GpuScene::record`, every upload frame | 15–23 ms, at the 64 MiB budget |
| A 4096² texture | `GpuScene::record`, its first level | 12–17 ms |

The copies themselves are cheap next to these (the `SceneUpload` pass is 30–39 ms with its staging allocation in it).

**Recommendation, in order:**

1. **Staging ring.** Each frame slot keeps one staging buffer of the upload budget's size, made once and mapped once; a frame's batch writes
   into its slot's buffer, which its fence guards. The per-frame allocation (15–23 ms) goes; a texture level larger than the budget gets a
   buffer of its own for that frame, as now. This is the plan's deferred "staging ring", whose trigger (staging allocations showing in the frame
   time) fired. Cost: `uploadBudget` × frame slots of host-visible memory held for the session (192 MiB at three slots).
2. **Pool sizing up front.** `Renderer::applySceneChanges` sums the vertices and indices of a list's added meshes, and the pool grows once, to at
   least that, before any `addMesh`, with headroom: 1.5× the size needed rather than doubling from the old size. A load then grows the pool a few
   times instead of once per change list that doesn't fit. On Sponza that should leave one or two growths, each in a frame with nothing else
   heavy. It doesn't make one growth cheaper: Caldera's last growth still allocates gigabytes in one frame.
3. **Large allocations off the render thread**, for what (1) and (2) leave. A growth or a large texture is made on a worker thread and handed to the
   render thread when ready; the uploads that need it wait in the queue meanwhile, and everything else goes on. This needs the RHI's buffer and
   texture creation to be safe from another thread (device memory allocation is, in Vulkan; the RHI's own bookkeeping, such as the allocation
   list the Memory window reads, would need a lock). That's an RHI change, so it is its own step, after (1) and (2) are measured.

I lean doing (1) and (2) now, as a phase 3 of this plan, and (3) only if Caldera still misses the target after them, which I expect.

Rejected:
- **Several fixed-size pool chunks.** Decision 4 rejected them because each chunk is another vertex and index binding, so the indirect draws
  would split per chunk. That still holds, and (3) removes the stall without it.
- **A lower upload budget.** Halving it halves the staging and copy cost per frame but doubles load time, and doesn't touch pool growth or
  texture creation, the larger costs.

**Verification for (1) and (2):**
- The longest frame while loading, in `SceneResident`, is under 50 ms on warm and cold Sponza, without `--no-wait-resident` changing anything
  else.
- No `UploadStaging` zone in a profile of a load, and at most two `GrowBuffer` zones on Sponza.
- three_cubes and Sponza byte-identical to `_out/depthrange-baseline/after`, as every phase so far.
- Caldera's longest loading frame recorded, as the number (3) starts from.

### Phase 3: loading responsiveness

1. **Staging ring** (`gpuscene.*`):
   - Each frame slot keeps one staging buffer of `uploadBudget` bytes, made at `init` and mapped once.
   - `record` writes the frame's batch into its slot's buffer, which the slot's fence guards, instead of making one per batch.
   - A batch larger than the budget (one texture level of more than 64 MiB) gets a buffer of its own for that frame, retired through the
     deletion queue as now.
   - `flushUploads` keeps its own buffers, since it has no frame slot.
2. **Pool sizing up front** (`renderer.cpp`, `gpuscene.*`):
   - `GpuScene::reserve(vertexCount, indexCount)` grows the pool once to at least the size in use plus what is asked, with 1.5× headroom over
     that rather than doubling the old size.
   - `Renderer::applySceneChanges` calls it with the sum of the list's added meshes, after the removals and before any `addMesh`.
   - The mesh and material tables grow the same way, from the list's highest handle and its material count.
3. **A delay flag for tests** (`src/apps/assetserver.cpp`, `server/assetserver.*`):
   - `--delay=<id pattern>:<ms>`, repeatable, holds an answer for matching ids (a glob over the asset id) that long before it is sent. The
     packing itself isn't delayed, only the answer.
   - It is traced at start-up and on each delayed answer, so a run can't be mistaken for a normal one. Documented in `src/asset/README.md` as
     a test aid.
4. **Docs.** `src/renderer/README.md` (the ring, reserve) and `src/asset/README.md` (the flag).

### Phase 3 verification

- **Sponza is responsive while loading.** The longest frame in `SceneResident` is under 50 ms, warm and cold (the cold scratch root, as in
  phase 2).
- **No staging allocation per frame.** A profile of a Sponza load has no `UploadStaging` zone.
- **The pool grows rarely.** At most two `GrowBuffer` zones on Sponza.
- **Caldera's capital recorded.** Its longest loading frame is recorded; above 50 ms, off-thread allocation goes to Deferred with that number
  as its trigger.
- **Placeholders.**
  - With `--delay='assets/main_sponza/textures/wood_door_01_BaseColor.png:3000'`, a `--no-wait-resident` screenshot after the scene's other
    assets have arrived shows the door white (the pending texture), and the final screenshot shows it textured.
  - With a material delayed, its prims draw in their display colour until it arrives.
- **Nothing else changes.** three_cubes and Sponza are byte-identical to `_out/depthrange-baseline/after`; debug and release build.

### Phase 3 results (2026-10-13)

Kept: the `--delay` test flag and the auto-framing fix. Reverted: the staging ring and `GpuScene::reserve`, after the measurements below.

What passes:
- **The staging ring.** A Sponza load has no `UploadStaging` zone; the slots' ring buffers are made once (`UploadStagingRing`, up to 9 ms each).
- **Pool reserve** is in. It doesn't reduce Sponza's growths much: Sponza arrives in about 40 small change lists, and a growth still doubles.
- **Placeholders.** With the asset server holding the door texture and the decal material for 8 s (`--delay`):
  - the render record during the hold has 27 textures and no cutout material;
  - the door's frame draws white, the pending texture, in the albedo view, and textured once it arrives;
  - the decal prims draw with their display-colour material until theirs arrives.
- **Nothing else changes.** three_cubes and Sponza in the lit, albedo, normals and prepass views are byte-identical to
  `_out/depthrange-baseline/after`, and Caldera's capital to its image.
- **Script camera moves end the auto-framing.** Found on the way: a `goto-asset` during a `--no-wait-resident` load was overridden by the camera
  following the scene. A camera changed by a script or RPC call now ends it, as hand input does.

What doesn't:
- **Sponza's longest loading frame is 80–104 ms** over four runs (85 ms before phase 3). Caldera's capital is 312 ms (435 ms before).
- **Why.** What remains is allocation on the render thread:
  - a pool growth up to 66 ms;
  - a 4096² texture's creation, 13–21 ms, in a frame that also copies 64 MiB.

  `vkAllocateMemory` on this GPU (an APU, memory shared with the system) zeroes large allocations before handing them over, so the cost grows
  with the size and the renderer can't avoid it on its thread. The recommendation expected (1) and (2) to bring Sponza under 50 ms; they don't,
  so the landing rule (Sponza meets the target) isn't met.

## Open questions

- The upload budget's default (64 MiB a frame) is a guess, to be measured on Sponza's 4096² textures (89 MB with mips each, so split by level).
- **Loading responsiveness** (decided 2026-10-13): landed with Sponza's longest loading frame at 80–104 ms and Caldera's capital at 312 ms;
  off-thread allocation is in Deferred.

## Deferred / follow-ups

- **Off-thread allocation** ((3) in "Responsiveness while loading"): large buffers (pool and table growth) and textures made on a worker thread
  and handed to the render thread when ready, the uploads that need them waiting in the queue meanwhile. Needs the RHI's buffer and texture
  creation to be safe from another thread (a lock on its allocation list at least), an RHI change. It is the cost that remains: a growth up to
  66 ms and a 4096² texture 13–21 ms on the render thread, which the main thread waits for (one snapshot in flight). Measured at landing: Sponza's
  longest loading frame 80–104 ms, Caldera's capital 312 ms. Trigger: streaming (it loads and evicts while the camera moves, so these frames show
  as hitches), or a frame budget that matters while loading. A staging ring and pool sizing ahead were tried first and didn't move the number.

- **A staging ring** (a fixed staging block per frame slot, reused) instead of a staging buffer per upload. Trigger: staging allocations
  showing in the frame time while Caldera loads.

- **Incremental composition.** Trigger: compose plus extract on the scene thread takes longer than a second for a typical change on Caldera at
  streaming scale.
- **Geometry pool compaction.** Trigger: fragmentation shows in the Memory window over a long session.
- **An async transfer queue.** Trigger: upload copies show up as GPU time in the frame graph's zones.
- **Residency and eviction**: the streaming steps take this path and add budgets, priorities and eviction.
