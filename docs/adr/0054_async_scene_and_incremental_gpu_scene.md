# 54. A scene thread and an incremental GPU scene: the view never waits

Date: 2026-10-13

## Status

Accepted

Amended by [ADR 67](0067_placement_tree.md): changes are no longer found by diffing whole-world extracts; a placement tree of immutable contents
gives them directly.

## Context

`ViewScene::open` blocked: it composed in rounds and waited on `AssetClient` for each asset. Changing a LOD level, a purpose or refreshing froze the
window. Every extract rebuilt the mesh and material libraries, so the renderer re-uploaded everything, and meshes were duplicated per placement.
On the GPU side, any geometry change rebuilt the whole geometry pool, and `GpuUploader` submitted and blocked; opening Sponza would have staged about
2.5 gigabytes in one frame. Streaming and LOD need meshes added and removed while the view runs.

## Decision

Scene side:

- A scene thread owns the composer, the packs and the libraries. It takes requests and asset arrivals and publishes an immutable snapshot plus a
  list of changes for the main thread.
- Identity is by asset: one mesh per mesh pack, one material per material asset or display colour, one texture per texture asset, refcounted and
  shared by every placement. A handle is never reused.
- Things draw as they arrive, with placeholders. Script frames count from when the scene is resident (`wait-resident`), so headless runs are
  deterministic.
- No start-up shaders: pipelines are requested at start but not waited for (amends [ADR 41](0041_pipeline_registry.md)).

GPU side:

- The geometry pool is suballocated with a first-fit free list that merges neighbours. When full it doubles and copies on the GPU, retiring the
  old buffer; there is no compaction. A size limit can be set; meshes that don't fit wait.
- Instances keep stable slots, and dead slots are reused.
- Changes apply in order: removals, then textures, meshes and materials, then draws.
- Uploads are copy passes in the frame's own command buffer, up to 64 megabytes per frame, each batch with its own staging buffer retired through
  the deletion queue. `GpuUploader` blocks only at start-up.

## Consequences

- Easier: frame time no longer depends on scene size or on what is loading; shared meshes save memory.
- Easier: long frames during loading are bounded (at most 24.8 milliseconds on a Sponza repack).
- Harder: the first design recomposed and diffed the whole world per batch — the cost that streaming v2 later removed.
- Harder: the pool fragments over a long session, and descriptor sets are still rewritten whole.
- Harder: main-thread responsiveness while loading was not fully met: 84.7 milliseconds worst on Sponza, 435 on Caldera.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_async_scene.md](../plan_async_scene.md), [plan_geometry_pool.md](../plan_geometry_pool.md),
  [plan_incremental_gpu_upload.md](../plan_incremental_gpu_upload.md) (superseded, never built), [src/view/README.md](../../src/view/README.md),
  [src/renderer/README.md](../../src/renderer/README.md) ("Scene data on the GPU").
- Code: `src/view/viewscene.cpp:69` (the scene thread), `src/scene/assetlibrary.h:8` (entries by asset key, handles never reused),
  `src/renderer/gpuscene.h:120` (`PoolAllocator`), `:151` (`defaultUploadBudget`, 64 megabytes), `:274` (`growPool`),
  `src/view/scenecommands.cpp:506` (the `scenethread` record).
- Differs from plan: the planned `src/view/scenethread.*` files don't exist; the thread lives in `viewscene.cpp`. Decision 2 (diff whole-world
  extracts) was replaced by [ADR 67](0067_placement_tree.md). This also supersedes the stage-2 geometry pool's full rebuild and
  [plan_tool_architecture.md](../plan_tool_architecture.md) decision 10 (pack loads applied on the main thread).
- Rejected alternatives: time-slicing on the main thread (compose and extract took 0.6 and 0.7 seconds on Caldera); incremental composition;
  several geometry pools (more draw calls); compacting instance slots; an async transfer queue (the RHI has one queue); a staging ring (tried,
  backed out).
