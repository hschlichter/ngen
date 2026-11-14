# 86. Large allocations off the render thread: tables grow ahead on background jobs, and what doesn't fit waits

Date: 2026-11-12

## Status

Accepted

## Context

While streaming, the main and render threads had frames of 100 to 300 ms. On a flight over the test world the render thread grew its GPU tables
when a change list outgrew them (the instance buffer, the culling buffers, the geometry pool, doubling and copying on the GPU as
[ADR 54](0054_async_scene_and_incremental_gpu_scene.md) decided), made a staging buffer per upload batch, and packed every changed draw record
on its own thread. A single `vkAllocateMemory` took up to 107 ms; on this machine (an APU) GPU memory is system memory, and large allocations
are slow. The job system (`src/jobsystem.*`) existed but had one kind of job, which `wait` ran on the waiting thread: a render thread helping
with a 200 ms allocation would make the spike it was meant to remove.

## Decision

Locked with Henrik on 2026-11-12 (the plan's decisions), and on 2026-11-13 (what waits, and the instance staging):

- **The job system is a library (`src/jobs/`) with two kinds of job.** Frame jobs are short pieces of the current frame (a parallel loop's
  chunks): the caller waits and runs them meanwhile. Background jobs are long (an allocation of hundreds of megabytes): their result is polled,
  `wait` never runs one, and the main and render threads never wait for one (teardown aside). Workers take frame jobs first. One pool for both,
  so the workers are shared.
- **Large buffers are made on background jobs, ahead of need** (`BufferGrowth`). A table that passes `render.growth.ahead` of its capacity
  (default 75%) starts its next buffer on a worker, at least half as large again; the render thread swaps it in, copying on the GPU, the frame
  after it is ready. RHI buffer and texture creation, mapping and destruction are safe from any thread; everything else stays on one.
- **When a table has outgrown its buffer and the larger one isn't ready, what doesn't fit waits; the frame doesn't.** Instance slots past the
  capacity stay dirty and aren't drawn, culling's command regions are cut to the capacity, and meshes wait in the pool's waiting list, as a
  mesh already waited for room. `GrowthBehind` and `GrowthReady` in the trace say when. Small tables (the mesh table, materials, LOD groups)
  grow at once.
- **Uploads go through a ring**: one mapped staging buffer per frame slot at the upload budget (64 MiB each), made once; a batch larger than the
  ring gets a buffer of its own, made in the background. The instance records have a staging of their own per slot, kept and grown ahead up to
  64 MiB; a larger change list makes its own buffer for that frame.
- **Render-thread loops proportional to a change list run as frame jobs**: packing the draw records, applying a list's slots (a chunk each, in
  list order), the culling groups' bounds. Changed slots are a bit per slot, read in order.
- **The target is counted in the engine**: the profiler counts each main- and render-thread frame's own work (its span less the outermost waits)
  against `profile.budget.frameMs`, in the `profile` record's `overBudget`, reset by `profile-reset`. A stall watch reports a thread that stays
  in one zone past `profile.stall.seconds`.

Rejected: blocking the render thread on the growth job (the spike again); growing on the render thread while a background growth is in flight
(three copies of a table at once, which on this machine ran out of memory); a second job pool for background work (workers idle while the
other pool is busy); doubling on growth (with growing ahead, `cull.commands` reached 2.5 GB); uploading part of a change list's records and
the rest next frame (changed draws would show old data for a frame, which breaks switching when complete, [ADR 61](0061_switch_when_complete.md));
a script over profile dumps as the measure (a 15-second window passed while the flight did not).

## Consequences

- Easier: on the flight, `vkAllocateMemory` on the render thread went from 107 ms to 0.1 ms in steady streaming, and the main thread stays
  under 12 ms.
- Easier: whether a run meets a frame budget is a record, with the zone that broke it, not a dump and a script.
- Harder: a jump larger than the headroom shows as a few frames where the new draws or meshes are missing (`GrowthBehind`).
- Harder: every growable table carries its next buffer while one is in flight, and the ring and staging are memory held all the time (on an
  APU, system memory).
- Accepted, not solved: the render thread still has frames over 16.7 ms on change lists of 250k to 410k instances (57 of 14,724 over 210
  seconds of flight, worst 85.5 ms): their records outgrow the 64 MiB instance staging, so the frame allocates (13 to 22 ms), and applying and
  packing such a list costs about 30 ms. Spreading a large change list over frames, or a larger staging, would address it.

## Verification

- Sources: [plan_streaming_spikes.md](../plan_streaming_spikes.md); `src/jobs/README.md`; `src/renderer/README.md` ("Growth", the upload ring,
  the instance uploads); `src/rhi/README.md` ("Threads"); `src/profile/README.md` ("Stalls", "Frames over budget").
- Code:
  - `src/jobs/jobsystem.h` (`JobKind`, `JobSystem::submit`, `wait`, `parallelFor`).
  - `src/renderer/buffergrowth.h` (`BufferGrowth`, `BufferGrowth::Behind`, `update`, `setAhead`); `src/renderer/renderconfig.h`
    (`growthAhead`).
  - `src/renderer/gpuscene.h` (`instanceGrowth`, `updatePool`, `uploadRing`, `instanceStaging`, `instanceStagingLimit`, `dirtyBits`);
    `src/renderer/drawlists.h` (`instanceGrowth`, `commandGrowth`, `workGrowth`); `src/renderer/lodgroups.h` (`growth`);
    `src/renderer/renderer.cpp` (`Renderer::growTables`, the `instanceCount` clamp, `CopyChangedRange`).
  - `src/profile/profile.h` (`laneFrameMark`, `overBudget`, `resetOverBudget`, `openZones`); `src/profile/rpc/stallwatch.h`;
    `src/view/profilecommands.cpp` (`profile.reset`).
- Differs from plan: the instance staging is kept per slot and grown ahead (the plan had one ring for everything); textures are still made on
  the render thread (`RhiCreateTexture` at most 6.6 ms); `JobWait` is in the Jobs group so a thread's own parallel loops count as its work; the
  full-flight verification was replaced by runs of about three and a half minutes of flight, and the target is not met for the largest change
  lists (Consequences).
