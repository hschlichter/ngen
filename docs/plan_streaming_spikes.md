# Plan: Spike-free streaming

**Status. Draft.**

## Current state

Flying the streaming test world ([test_streaming_world.md](test_streaming_world.md)) with `fly /cameras/flight_ 50` in a release build (about
28 km in 9 minutes, 34k frames) gives a median frame of 3.3 ms, p99 132 ms and a worst of 2.5 s; 551 frames are over 100 ms. With the markers
added for this ([src/profile/README.md](../src/profile/README.md), "How deep"), every spike reads down to its cause. Nearly all main-thread
spikes are `WaitRenderThread`: the main thread runs one snapshot ahead of the render thread ([ADR 8](adr/0008_render_thread_with_snapshots.md)),
so a long render-thread frame is a long frame. Measured on the second flight (worst single call):

| # | Render thread | Worst | Cause |
|---|---|---|---|
| R1 | `PackInstanceRecords` | 264 ms for 384k records | each culling record is built by reading the draw record back from the mapped staging buffer (`CpuToGpu`, uncached) |
| R2 | `PackInstanceRecords` after `InstanceBufferGrown` | 1.94 s for 2.77M records | a grown instance buffer is new and empty, so every record is packed and uploaded again (`allDirty`) |
| R3 | `vkAllocateMemory` under `GrowInstanceLists`, `GrowMeshletWork`, `UpdateInstances`, `GrowBuffer`, `ApplyLodGroups` | 20–236 ms each, about ten in one frame | large GPU buffers made on the render thread when a table outgrows its buffer |
| R4 | `SceneBounds` | 84 ms | the union of every instance's bounds, scanned on every change list |
| R5 | `ResizeSlots` | 68 ms | `Renderer::gpuInstances` (a `std::vector`) reallocates and copies hundreds of MB when the slot count grows |
| R6 | `WaitSceneChangesApplied` (main thread) | 43 ms | the render thread holds `uploadMutex` while it applies a change list, so the main thread's next `submitSceneChanges` blocks |
| R7 | `UploadStaging`, `RhiCreateTexture` | 80 ms, 51 ms | a staging buffer made for every upload frame; a 4096² texture made when its first level uploads |
| R8 | `UploadCopies`, `CopyChangedSlots` | 101 ms, 40 ms | per-upload packing (meshlets and positions converted, bytes copied) and per-slot copies, on one thread |

On the main thread: `UpdateSceneBounds` (21 ms, the cascades' scene bounds scanned per chunk) and the selection's bounds (`ViewScene::bounds`,
every draw scanned; carried over from uncommitted work, see step 1).

R3 and R7 are the deferred "off-thread allocation" of [plan_async_scene.md](plan_async_scene.md) (Deferred), whose trigger was "streaming (it
loads and evicts while the camera moves, so these frames show as hitches)". That trigger has fired.

The job system (`src/jobsystem.*`, [plan_job_system.md](plan_job_system.md)) starts 23 workers in ngen-view that nothing uses: one queue under a
mutex, `std::function` jobs, a `JobFence` per job, and a `wait` that runs queued jobs while it waits. It is compiled into the `ngen-view`
program rather than a library, so the renderer and view libraries can't link it.

## Scope

In:
- Frame spikes from streaming on the main and render threads: R1–R8 and the main thread's bounds.
- The job system as the place off-thread work runs: a library, two kinds of job, a parallel loop.
- Measuring spikes in the engine, so the target is a record and not a script over profile dumps.

Out:
- The scene thread's latency (it doesn't lengthen frames; it makes levels arrive late: 379 late switches, 278 root-seconds coarser on the
  flight). Its own plan, with the job system this one builds; the numbers are in Deferred.
- GPU frame time (p90 33 ms on the flight is `Opaque` and `Cutout two-sided` draws: what the content costs to draw, not streaming).
- Spreading a large change list over several frames (conflicts with switching when complete, [ADR 61](adr/0061_switch_when_complete.md)). Only
  if what is left after this plan misses the target; see Deferred.

## Decisions

Locked (2026-11-12):

1. **Frame spikes only.** The scene thread's latency is its own plan.
2. **Large allocations off the render thread**, as [plan_async_scene.md](plan_async_scene.md) proposed: a worker makes the buffer or texture,
   the render thread takes it when it is ready. Growth starts early, so the new buffer is usually ready before it is needed.
3. **The job system runs it**, and the plan looks for other render- and main-thread work to move onto the workers (steps 5 and 6).
4. **Target: 16.7 ms.** No main- or render-thread frame does more than 16.7 ms of its own work during the flight (see Verification).
5. **`PlacementBounds` carried over** from the uncommitted work in the main checkout, and used for the scene's bounds too.

Leaning, pushback welcome:

6. **Two kinds of job.** *Frame* jobs are short pieces of the current frame's work (a parallel loop's chunks); the caller waits for them, and
   helps. *Background* jobs are long (an allocation of hundreds of MB); nobody waits for them, their result is polled, and `wait` never runs one:
   a render thread that helped with a 200 ms allocation while waiting for its own loop would make the spike it is meant to remove. Workers take
   frame jobs first. I lean two queues in the one system over a second pool, so the workers are shared.
7. **A needed buffer that isn't ready waits, the frame doesn't.** When a table has outgrown its buffer and the bigger one is still being made,
   what doesn't fit waits for it, as a mesh waits for room in the geometry pool today: instance slots past the capacity aren't uploaded or drawn,
   a texture's levels stay queued. Growing at 75% full makes this rare; a jump larger than the headroom (a tile adding 70k–380k instances at
   once) shows as a few frames where those draws are missing. Against: blocking the render thread on the job, which is the spike again.
8. **Staging ring for uploads** (R7): one staging buffer per frame slot at the upload budget, made once (192 MiB at three slots), instead of a
   background allocation per frame. It was tried in [plan_async_scene.md](plan_async_scene.md) and backed out because it didn't move Sponza's
   worst frame, which growths dominated then; with growths off the render thread it is what is left. A batch larger than the ring (a texture
   level over the budget) gets a buffer of its own, made in the background.
9. **The job system moves to `src/jobs/`**, a library (`jobs`) with its README, linked by the renderer and view libraries and the programs that
   use them. Against: keeping `src/jobsystem.*` in `src/` as a cross-cutting file; but it now has rules (two kinds, who may wait) that need a
   README.

## Steps

Each step ends with a short run aimed at the pattern it removes (a stretch of the flight that shows it, one profile dump of the last
seconds), not the whole map: the patterns repeat everywhere, and a whole-map flight's dumps outgrow the scratch space (`/tmp` is a RAM-backed
tmpfs). The numbers are recorded here. The full flight comes back once, at the end, for Verification.

### 1. Locks, the pack loop and the bounds (R1, R4, R6, the main thread's bounds)

- **R6.** `RenderThread::threadLoop` takes `pendingChanges` under `uploadMutex` and applies it after unlocking:

  ```cpp
  std::optional<SceneChanges> changes;
  {
      std::lock_guard lock(uploadMutex);
      appliedUpTo = submitted.load(std::memory_order_acquire);
      changes = std::exchange(pendingChanges, std::nullopt);
  }
  if (changes) {
      PROFILE_ZONE("SceneChanges", Render);
      renderer->applySceneChanges(*changes);
  }
  ```

  `appliedUpTo` keeps its meaning: what was taken is applied before this frame's render.
- **R1.** `GpuScene::addInstanceUploadPasses` builds the `GpuInstanceRecord` in a local, writes it to the staging buffer, and builds the
  `GpuCullRecord` from the local. Nothing reads mapped memory.
- **Bounds.** Carried over from the main checkout: `PlacementBounds` (`src/view/placement.*`) answers `ViewScene::bounds(path)` by walking the
  published placement tree, keeping each placement's bounds from one snapshot to the next; `ToolsHooks::selectionBounds` works the selection's
  bounds out once per snapshot. Then:
  - `ViewFrame::fitCascades` takes the scene's bounds from `scene.bounds("/")` instead of `SceneBoundsCache::update`, which re-reads every
    chunk of the instance list a change list replaced (21 ms after a large one). `SceneBoundsCache` stays for the ground test, whose per-chunk
    bounds are its acceleration, and is updated only while ground following is on, as now. If `PlacementBounds` measures no cheaper on the
    flight (it also walks what a snapshot replaced), the cascades keep `SceneBoundsCache` and its update becomes a `parallelFor` in step 6.
  - The snapshot carries those bounds (`RenderSnapshot::sceneBounds`); `Renderer::applySceneChanges` drops its scan (R4), and the renderer's
    single-cascade fallback and the cascade debug lines read the snapshot's.

**Done 2026-11-12.** Measured on two full flights (release, `fly /cameras/flight_ 50`), against the second flight in Current state:

- `WaitSceneChangesLock` (renamed from `WaitSceneChangesApplied`: the wait is for the lock only now): 43 ms worst before, 0.02 ms after.
- `PackInstanceRecords`: 1.94 s worst before (2.77M records after a growth), 81 ms after; 264 ms for 384k records before, under 64 ms after.
- The render thread's `SceneBounds` scan (84 ms) is gone.
- Main frames: p99 132 ms before, 43 ms after; worst 2.5 s before, 447 ms and 1,988 ms on the two flights after (the second is two 270 MB
  `vkAllocateMemory` calls of 685 and 574 ms: step 5).
- **The cascades took the fallback.** `scene.bounds("/")` cost up to 106 ms a frame on the main thread (every placement a snapshot replaced is
  walked again, the instance placements under a new tile among them), against `SceneBoundsCache`'s 15–21 ms. The cascades keep
  `SceneBoundsCache`, and its result is what the snapshot hands the renderer. `PlacementBounds` answers the selection, framing and go-to only.
- Screenshots: `three_cubes`, `layered` and the test world at `/cameras/flight_02` byte-identical to the build before; `/cameras/flight_12`
  differs from run to run with either build, and one run after matched one before byte for byte.
- **Not explained: late switches rose** from 379 to 518 and 538 (root-seconds coarser 278 to 625 and 682). The scene thread did the same work
  (loads, releases, walks built, contents unloaded all equal) in about the same total time, but in half as many batches, each about twice as
  long, and a level that arrives waits for the batch in progress. A flight of the build before, to separate this change from the conditions
  (memory on the tmpfs filling with dumps, the main and render threads running uncapped and freer), was lost to a full scratch volume. It
  belongs to the scene-thread plan (Deferred); open question 3.
- **A start-up hang, in both builds, not caught yet.** Opening the test world at `/cameras/flight_02` or `/cameras/flight_12` hung 3 times in
  about 25 runs (twice with the commit before this step, once after step 4): about 10 s into the load the main thread blocks and stays blocked
  (0.2 s of CPU in ten minutes), the render thread runs at a full core in user space for as long as the process lives, the scene thread idles,
  and there is no GPU reset in the kernel log. It never hung under `gdb` (5 runs) or in a hunt of 10 runs sampling the render thread with
  `perf_event_open`. Open question 4.

### 2. Spikes measured in the engine

Moved after step 6 (2026-11-12): the steps between are checked by short runs aimed at their pattern; this is what the final flight reads.

The target is read from a record, not from a script over profile dumps:

- The collector keeps, per lane that marks frames (Main, and Render through a new `profile::laneFrameMark()` once per render-thread
  iteration), each frame's own work: its span less the outermost Wait-group zones in it (`WaitRenderThread`, `SubmitSnapshot`, `WaitSnapshot`,
  `Present`, fence and lock waits).
- Since start or `profile-reset` (a new verb and `profile.reset` method): the frames over `profile.budget.frameMs` per lane, the worst frame's own
  time, when, and its longest non-wait zone (name, `file:line`).
- `profile` record: `overBudget: {lane: {frames, worstMs, worstAt, worstZone}}`. Performance window: the same, under the frame times.

### 3. The job system as a library, two kinds of job, a parallel loop

- `src/jobsystem.*` moves to `src/jobs/jobsystem.*` (`git mv`), library `jobs` in `build.cpp` (links `profile`); `renderer` and `view` link it;
  `ngen-view` stops compiling it itself. `src/jobs/README.md`: what runs there, the two kinds, who may wait for what, where it sits in the frame.
- Two queues (decision 6):

  ```cpp
  enum class JobKind : uint8_t {
      Frame,      // short, part of this frame's work; waited for, and the waiter helps
      Background, // long; polled with JobFence::ready(), never waited for on the main or render thread, never run by a waiter
  };

  static auto submit(JobFunc job, const char* name = "Job", JobKind kind = JobKind::Frame, std::source_location location = std::source_location::current()) -> JobFence;
  ```

  A worker takes a frame job when there is one, else a background job. `wait` runs only frame jobs while it waits.
- A parallel loop, for steps 5 and 6:

  ```cpp
  // Runs body(begin, end) over [0, count) in chunks of at least `grain`, on the workers and the caller; returns when every chunk has run.
  static auto parallelFor(size_t count, size_t grain, const std::function<void(size_t, size_t)>& body, const char* name, std::source_location location = std::source_location::current()) -> void;
  ```

- A `jobs` record: workers, frame and background jobs queued and running, completed since start, the longest background job running and for how
  long.

**Done 2026-11-12.** `src/jobs/` with its README; the `jobs` record in `src/view/profilecommands.cpp`. Nothing submits to it yet; steps 5 and 6
are its first users. On the way: a waiter could miss the notify of a job finishing between its check and its wait (the done flag was stored
without the queue lock); the worker now takes the lock before notifying.

### 4. Instance growth without a repack (R2, R5)

- **R2.** `GpuScene::ensureInstanceCapacity` no longer sets `allDirty` when there are records to keep: the old buffer's contents are copied into
  the new one on the GPU before this frame's instance upload (`addInstanceUploadPasses`: a copy of `instanceCapacity` old slots' draw records and
  culling records, which sit at different offsets in the new buffer), as the geometry pool grows ([ADR 54](adr/0054_async_scene_and_incremental_gpu_scene.md)).
  Only the changed slots are packed. `InstanceBufferGrown` says what was copied.
- **R5.** `Renderer::gpuInstances` becomes a `ChunkedList` (`src/renderer/renderworld.h`, the list the view's instances already use: chunks
  that never move), so growth adds chunks and copies nothing. Its readers (`DrawLists::updateGroupBounds`, `GpuScene::updateInstances` and the upload's pack loop) take it by
  chunk. Open question 1 has the alternative.

**Done 2026-11-12.** The pass is `InstanceMove` (`addInstanceUploadPass`, now named per use). Every reader of the instance list takes the
`ChunkedList` by reference: the three passes' draw-log report, `DrawLists`, `GpuScene`, and `RendererState::instances` (a pointer) for the
render tools; the passes' lambdas hold a pointer, never a copy, which would make the next edit copy whole chunks. `ChunkedList` gained
`resize`. Measured on opening the test world at `/cameras/flight_02` (the instance buffer grows twice while it loads, the second time copying
1.6M slots), against the commit before step 1:

- Worst main frame 1,854 ms before, 456 ms after; `PackInstanceRecords` 1,422 ms worst before, 71 ms after.
- `InstanceMove`: 6.8 ms of GPU time for the 1.6M slots.
- `ResizeSlots` 32 and 63 ms (one run each): no longer a copy, but the first touch of the 1.6M new slots' memory (about 240 MB) at load.
- What is left over 50 ms is allocation: `InstanceStaging`'s staging buffer of its own (88 ms) and the growths in `SceneChanges` (step 5).
- Screenshot at `/cameras/flight_02` byte-identical to the commit before step 1, after both growths: the copy carries every slot.

### 5. Allocations on background jobs (R3, R7)

- A `GrowthJobs` helper in `src/renderer/` (`growthjobs.*`): `start(key, descs)` submits a background job that makes and maps the buffers,
  `take(key)` returns them once its fence is ready. Debug names are copied into the job. The RHI documents which calls are safe from another
  thread (`createBuffer`, `createTexture`, `mapBuffer`; the allocation registry already locks) in `src/rhi/README.md`, and they are made so
  where they aren't.
- Each growable starts its growth at 75% full (`render.growth.ahead`, a configuration value, default 0.75) and swaps when the job is ready, in
  the frame it takes it: the instance buffer (step 4's copy), `DrawLists` (its per-slot buffers grown together, one job), `LodGroups`, the
  geometry pool and tables (`GpuScene::ensureCapacity`, the pool's copy as today), the meshlet work items.
- Past the capacity before the job is ready (decision 7): `Renderer::applySceneChanges` uploads and draws slots up to the capacity, and keeps the
  rest dirty; meshes wait in the pool's waiting list; the frame's instance count for the culling passes is the capacity. A trace warning
  `GrowthBehind` (what, needed, capacity) when that happens, `GrowthReady` when it clears.
- Textures: `GpuScene::record` starts a background job for a texture's GPU image when the texture is queued (not when its first level is
  recorded); its levels stay in the queue until the image is ready.
- Staging ring (decision 8): `GpuScene::record` writes into the frame slot's staging buffer, made once at the upload budget's size.

### 6. Render-thread work on frame jobs (R8, and what steps 1–5 leave)

Measured first: only what still shows over 2 ms in a flight frame moves. The candidates, each a `parallelFor` over independent items:

- `PackInstanceRecords`: chunks of slots, each writing its own staging range (regions per run of slots are merged after).
- `UploadCopies`: the batch's uploads, each packing into its own staging offset (offsets computed first); the copy commands recorded after.
- `CopyChangedSlots` and `UpdateGroupBounds`: chunks of slots or groups; the bucket counts (`DrawLists::countInstance`) summed per chunk and
  added after.

Not moved: frees (`RhiDestroyBuffer` at most 1.6 ms), `CopyLodGroups` (1.6 ms), the frame graph's recording (one command buffer).

## Visibility

- **Records:** `profile` gains `overBudget` (step 2); a new `jobs` record (step 3); `gpuscene` gains `growth`: per growable its capacity, use,
  a growth in flight (bytes, since when) and what waits for it (slots, meshes, texture levels); `memory` lists the staging ring.
- **RPC and verbs:** `profile.reset` and `profile-reset` (step 2). `render.growth.ahead` is a configuration value (`config` verb and record,
  Configuration window).
- **Trace:** `InstanceBufferGrown` (now with what was copied), `GrowthBehind` (warning) and `GrowthReady`.
- **Windows:** the Performance window shows `overBudget` per lane and the worker lanes with frame and background jobs in their group colours
  (they are there today); the Memory window shows growths in flight and the staging ring.
- **Where it sits in the flow:** `src/jobs/README.md` (new), `src/renderer/README.md` ("Scene data on the GPU": growth, staging),
  `src/profile/README.md` (`overBudget`), `src/rhi/README.md` (which calls are safe from another thread).

## Verification

The flight, release build, from the repository root with the release asset server running:

```
0 config profile.history.seconds 120
1 profile-reset
1 streaming-reset
2 fly /cameras/flight_ 50
3 record profile <records>
3 record streaming <records>
3 record gpuscene <records>
10 quit
```

- `profile` record, `overBudget`: Main and Render each 0 frames over 16.7 ms. This is the plan's target (decision 4).
- `WaitSceneChangesApplied`: no call over 1 ms in the dumped history (step 1).
- No `PackInstanceRecords` call over 2 ms; `InstanceBufferGrown` reports a copy, not a full upload (step 4).
- No `vkAllocateMemory` on the Render lane after the first `SceneResident` (step 5); `GrowthBehind` is reported, and when it is, `GrowthReady`
  follows.
- `streaming` record not worse than the baseline: `holes` 0, `lateSwitches` at most 379, `rootSecondsCoarser` at most 278 (the scene thread is
  untouched; a regression would mean the render side is holding streaming back).
- The trace: no warnings or errors apart from `GrowthBehind`.
- Byte-identical screenshots of `three_cubes` and Sponza at the default camera (`--frames=80`, headless) before and after.
- The RHI examples pass with `--check --validation` (`timestamps` and `gpuzones` failed before this plan too, and are not counted).
- `jobs` record during the flight: background jobs complete, and none runs longer than the flight's longest growth.
- The selection's bounds: `/World/Regions` selected on the test world, `SelectionBounds` under 0.2 ms per snapshot after the first.

## Open questions

1. **R5: chunked list or reserve ahead.** A `ChunkedList` for `gpuInstances` copies nothing on growth but changes three readers from a span to
   chunks. Reserving with headroom (doubling ahead on a background job, then moving the elements on the render thread) still copies once per
   growth. I lean the chunked list.
2. **The 75% growth threshold** is a guess; the flight's largest single change list (380k instances, 14% of 2.77M slots) fits in the headroom
   it leaves at that size, not at 1.6M. The `gpuscene` record's growth history will say.
3. **Late switches after step 1** (see its results): this step, or the conditions it was measured in. Settled by a flight of each build under
   the same conditions, with the scene-thread plan.
4. **The start-up hang** (step 1's results): where the render thread spins. It hides from a debugger and from hunting; the engine could say
   itself, with a stall watchdog that traces the open profile zones of a thread that has been in one frame for seconds.

## Deferred / follow-ups

- **Scene-thread latency** (its own plan). Measured on the flight: `SyncInstancer` (196 ms, one per point instancer; instancers could place in
  parallel), `SceneRetire` (195 ms of freeing old snapshots, which a background job can do), `CountEntities` (379 ms, a whole-world visit for a
  trace message), `ComposeLevelPrims` (135 ms), `AssetReceive` (52 ms; layer and mesh packs could be decoded on workers before the scene
  thread takes them). Trigger: this plan landed.
- **Spreading a large change list over frames.** Trigger: after step 6, a flight frame over 16.7 ms whose time is proportional to one change
  list's size. Needs a way to keep a level switch whole ([ADR 61](adr/0061_switch_when_complete.md)).
- **A suballocator** (fewer, larger `vkAllocateMemory` calls). Trigger: background growth jobs too slow to keep up with streaming
  (`GrowthBehind` frequent on the flight).
