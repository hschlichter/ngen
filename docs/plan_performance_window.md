# Performance window and profiling markers

**Status. Landed.**

## Current state

Timing today: GPU per-pass timestamps in the frame graph (`docs/plan_gpu_timestamps.md`), shown in the frame graph window and as a `GpuTime`
observation. Nothing on the CPU side: no frame time, no stage timing, no idea whether the main thread, the render thread or the GPU is the
bottleneck. The Sponza numbers (`notes.md`) came from headless obs dumps and hand-computed medians.

The obs bus (`obs.md`) is the only instrumentation. It is an event log for behaviour, not a profiler: emitting one observation per zone per
frame would flood it, and it has no notion of scope, thread lanes or GPU time.

Threads: main (input, scene updates, editor UI, snapshot build) and render (upload, frame graph build, record, submit, present). The GPU is a
third lane. A frame on the main thread and a frame on the render thread overlap by design.

## Goals

1. **Markers**: one API to bracket a stage on any thread and on the GPU, cheap enough to leave in shipping code.
2. **Performance window**: FPS and frame time history, and a timeline of the last frame with one lane per thread and one for the GPU.
3. **Tracy-ready**: the marker API is shaped so a later Tracy integration is a second sink behind the same macros. Tracy itself is a
   separate plan.

## Scope

**In**

- `src/profile/`: marker API, collector, frame assembly. Header-only macros, small `.cpp` for the collector.
- CPU instrumentation of the main loop and the render thread at stage granularity (about 15 zones total).
- GPU zones through the RHI, replacing the frame graph's own timestamp bracketing: `cmd->beginGpuZone(name)` / `endGpuZone()`.
- `Performance` window in the editor: stats header, frame-time plot, timeline.

**Out**

- Tracy: vendoring, `TracyClient`, `TracyVkCtx`. Own plan, `docs/plan_tracy.md`, once this one has landed; the macros here gain a second
  expansion then and nothing else changes.
- Memory profiling, allocation tracking, lock contention. Tracy territory.
- Calibrated CPU/GPU clock alignment in the built-in window. See fork 4.
- Persisting timelines to disk. Trigger: a headless perf regression check; would go through the obs bus as a per-frame summary.

## Decision forks

Each with options and a recommendation. Locked once answered.

### 1. Marker API shape

- **A. Own macros, built-in collector, room for a second sink (recommend).** `PROFILE_ZONE("name")`, `PROFILE_ZONE_VALUE(v)`,
  `PROFILE_FRAME_MARK()`, `PROFILE_GPU_ZONE(cmd, "name")`. Each expands to the built-in collector call; a future `NGEN_TRACY` build adds the
  matching Tracy macro in the same expansion. The window never depends on Tracy.
- **B. Tracy macros directly, window reads Tracy.** Not possible: the Tracy client streams to the server and exposes nothing locally.

### 2. GPU zones: where they live

- **A. RHI command buffer API (recommend).** `beginGpuZone(const char*)` / `endGpuZone()` on `RhiCommandBuffer`, same rank as `beginLabel`.
  Backend writes timestamps into a per-command-buffer query allocation it manages and records the zone stack. Results read by
  `device->collectGpuZones(cmd)` after the fence: `{name, startNs, endNs, depth}`. Replaces `setTimestampPool` and the renderer's per-slot
  pools from the timestamps plan; the frame graph calls the zone API around each pass, and passes can nest their own. A later Tracy
  integration adds `TracyVkZone` inside the same backend calls.
- **B. Keep raw query pools in the renderer as they are.** Works, but zones cannot nest and every pass that wants a sub-timing would
  reinvent the bookkeeping.

### 3. Timeline data model

- **A. Per-thread ring buffers of completed zones, frame assembled on demand (recommend).** Each thread pushes `{nameId, start, end, depth}`
  into its own lock-free ring on zone end. `PROFILE_FRAME_MARK` on the main thread records frame boundaries. The window asks the collector for the
  zones of every lane intersecting the selected frame's interval. No allocation per zone, no locks on the hot path. History is sized for
  scrubbing: 1024 frames and 16384 zones per lane, several seconds at editor frame rates; GPU zone sets are kept per frame and matched to
  a CPU frame by the submit time the renderer records.
- **B. Zones sent to the render snapshot.** Ties profiling to the render pipeline; the render thread's own zones would lag a frame.

### 4. GPU lane alignment

- **A. Separate origin.** GPU lane drawn on its own time axis. Shipped first; replaced once the timeline became pannable, because
  seeing the main-to-render-to-GPU delay was the point.
- **B. `VK_EXT_calibrated_timestamps` (landed).** `RhiDevice::calibrateGpuClock` samples the GPU and CPU monotonic clocks together;
  the renderer recalibrates every 120 frames and converts every GPU zone to the CPU clock before handing it to the profiler. Devices
  without the extension anchor the GPU frame at its submit time, a lower bound, and the window says so.

### 5. First stages to instrument

Main thread: `PollEvents`, `SceneUpdate`, `EditorUI`, `BuildSnapshot`, `SubmitSnapshot` (includes the wait for a free slot).
Render thread: `WaitSnapshot`, `Upload`, `WaitFence`, `Acquire`, `BuildFrameGraph`, `Compile`, `Record`, `Submit`, `Present`.
GPU: one zone per frame graph pass (from the graph), `Upload` batches.

## Steps

1. **Collector and macros.** `src/profile/profile.h`:

   ```cpp
   namespace profile {
   struct Zone { uint32_t nameId; uint64_t startNs; uint64_t endNs; uint16_t depth; };
   auto registerName(const char* name) -> uint32_t;          // interned, once per static string
   auto beginZone(uint32_t nameId) -> void;                  // this thread
   auto endZone() -> void;
   auto frameMark() -> void;                                 // main thread
   auto lanes() -> std::span<const LaneInfo>;                // one per registered thread
   auto zonesInFrame(uint32_t laneIndex, FrameInterval, std::vector<Zone>& out) -> void;
   auto lastFrames(std::span<FrameStats> out) -> size_t;     // frame times for the plot
   }
   #define PROFILE_ZONE(name) ... // RAII object; a Tracy expansion joins here later
   #define PROFILE_FRAME_MARK() ...
   ```

   Per-thread state is a `thread_local` pointer to a lane registered on first use; lanes live in a fixed array (8). Ring size 4096 zones per
   lane. Clock: `std::chrono::steady_clock`, same base as obs `ts_ns`.

2. **GPU zones in the RHI.** `RhiCommandBuffer::beginGpuZone(const char*)`, `endGpuZone()`; `RhiDevice::collectGpuZones(RhiCommandBuffer*,
   std::vector<RhiGpuZone>&) -> bool` (false until the fence has passed). Vulkan: a query pool per command buffer, 128 timestamps, reset at
   `begin()`, zone stack in the command buffer object. `createQueryPool` and friends stay as the low-level path the zones are built on;
   the `timestamps` example uses them.

3. **Frame graph and renderer.** Frame graph brackets each pass with `PROFILE_GPU_ZONE(cmd, passName)` instead of raw timestamps. Renderer collects
   GPU zones after the slot fence and hands them to the profiler as the GPU lane (`profile::submitGpuZones(frameId, zones)`). The
   `FrameGraphDebugSnapshot` GPU fields stay, fed from the same data.

4. **Instrumentation.** The stages from fork 5, in `main.cpp` and `renderthread.cpp` / `renderer.cpp`.

5. **Performance window.** `src/ui/performancewindow.*`, menu entry next to Frame Graph. Header: FPS, frame ms, GPU ms. The timeline is
   one continuous CPU-clock axis, Tracy-style: drag to pan, wheel to zoom around the cursor, a frame ruler on top, main, render and GPU
   lanes below, so the delay from snapshot to render to GPU is visible. Live follows the newest frame; any pan, zoom or click pauses
   collection; Live and Follow return.
   Plot: frame-time history with a 16.6 ms guide; click a frame to select it. Scrub slider and step buttons over the whole history, with
   seconds-back readout; Pause/Live button, which also pauses collection so the pinned frame is never evicted. Timeline: one row per lane,
   zones as rectangles with name and ms, nesting by depth, hover tooltip. Click a zone for a detail pane: self time, children with share of
   parent, ancestry, share of frame, optional value (`PROFILE_ZONE_VALUE`, one `uint64` per zone), and min/avg/p99/max over the history for
   that zone name on that lane. Custom `ImDrawList` drawing.


## Verification

- Headless three_cubes run: a `FrameStats` observation every 60 frames with main, render and GPU ms; values in the ranges the timestamps plan
  measured; validation clean. Verified: `main_ms` about 1.9, `gpu_ms` about 1.1, one field per top-level zone on both lanes (for example
  `Main.SubmitSnapshot`, `Render.Present`, `Render.Record`).
- Performance window shows FPS, plot and three lanes with named zones; pause freezes them; hover shows ms. Henrik, visual.
- Sponza: window shows GeometryPass and ShadowPass as the two widest GPU zones, matching `notes.md`.
- Example sweep unchanged; a new `gpuzones` example asserts nesting and ordering of `beginGpuZone` results. Verified. The base gained a
  `slotReady(slot, cmd)` hook because zones must be collected before the command buffer's next `begin()` clears them.

## Deferred / follow-ups

- **Tracy** (`docs/plan_tracy.md`): submodule, `TracyClient` static library under debug and release, `NGEN_TRACY` expansion in the macros,
  `TracyVkCtx` in the Vulkan backend behind `beginGpuZone`. Trigger: this plan landed and a question the overview cannot answer.
- Calibrated GPU alignment (fork 4B).
- Zone statistics over N frames (min/avg/max per zone name) in the window. Trigger: first regression hunt.
- Headless perf regression check comparing `FrameStats` against a stored baseline. Trigger: first regression that slipped through.
