# GPU counters

**Status. Landed.**

Stage 5 of [plan_introspection.md](plan_introspection.md). It builds on the debug names ([plan_debug_names_and_memory.md](plan_debug_names_and_memory.md)).

## Current state

Every frame-graph pass is a GPU timing zone (`PROFILE_GPU_ZONE` in `FrameGraph::execute`), read back in `Renderer::readGpuTimings` and shown as one
number per pass in the Frame Graph and Performance windows. Since the GPU-driven stages, each pass draws with a few `drawIndexedIndirectCount` calls, so
per-draw timing is gone, and nothing says how much work a pass made the GPU do: vertex and fragment invocations, clipping, primitives, compute
invocations. There is no history of any counter beyond the Performance window's frame time.

## Scope

**In**

- **Pipeline statistics (RHI).** `RhiDeviceLimits::pipelineStatistics`. `RhiCommandBuffer::beginPipelineStats(name)` and `endPipelineStats()` count
  the work recorded between them. `RhiDevice::collectPipelineStats(cmd, out)` returns an `RhiPipelineStatsZone` per pair after the fence:
  input-assembly vertices and primitives, vertex-shader invocations, clipping invocations and primitives, fragment-shader invocations, and
  compute-shader invocations.
  - Vulkan: one `VK_QUERY_TYPE_PIPELINE_STATISTICS` pool per command buffer (64 queries), reset on the first use in a recording. The device enables
    `pipelineStatisticsQuery` when supported.
  - They don't nest (Vulkan allows one active query of a type), and begin and end sit outside `beginRendering`/`endRendering`.
- **Per pass.** `FrameGraph::setPipelineStatsEnabled(bool)` wraps each pass's execute in a pipeline statistics query when on. Only on while the
  Counters window is open or a dump is pending: the queries are cheap but not free.
- **GPU zones per indirect call.** Each `drawIndexedIndirectCount` in the depth prepass, geometry pass and shadow pass sits in a nested GPU zone named
  by its region (`camera.single`, `camera.double`, `cascade0.single`, …). Always on, like the pass zones. The zones reach the profiler's GPU lane as
  well.
- **`GpuCounters`** (`src/renderer/gpucounters.h`): per frame, the GPU frame time, every zone (name, depth, ms), and per pass the pipeline
  statistics. Queued per frame through `RenderThread` while enabled (at most 300), so the history has no gaps.
- **Counters window** (Windows > Introspection > Counters): a table of every pass's time and statistics, the nested zones under each pass, derived
  ratios (fragments per pixel, primitives surviving clipping, vertices per primitive), and a history plot (last 300 frames) of the selected counter.
- **Dump and verb:** `dump-counters PATH` writes the next `GpuCounters` as JSON. An obs event `Render/PipelineStats` per pass, while enabled.

**Out**

- Vendor hardware counters (occupancy, cache hit rates): `VK_KHR_performance_query` is not portable and needs profiling locks. Tools like Nsight and
  Radeon GPU Profiler cover it, with stage 1's names.
- Pipeline statistics per indirect call: queries don't nest, and the pass already has one.

## Decisions

Made while planning, following the umbrella's recommendations.

- **Statistics per pass, time per indirect call.** Time zones nest, so each region gets one inside its pass. Statistics queries don't, so they stay
  at pass level where one query covers every draw.
- **The backend owns the queries**, as it does for GPU zones. Passes name a zone; they never see a query pool. This keeps the frame graph's use to
  two calls.
- **History lives in the window**, on the main thread. The render thread sends one frame at a time; the window keeps a ring of 300 samples per counter.
- **Region zones are always on.** Twelve more timestamp pairs a frame is noise, and it keeps the GPU lane in the profiler complete.

## Steps

1. RHI: `RhiPipelineStats`, `RhiPipelineStatsZone`, `RhiDeviceLimits::pipelineStatistics`, the command buffer and device calls. Vulkan: feature,
   pool per command buffer, lazy reset, collection with availability.
2. Frame graph: `setPipelineStatsEnabled`; begin and end around each pass's execute.
3. Passes: region zones around the indirect calls (`DrawLists::regionName(region)`).
4. Renderer: `GpuCounters` built in `readGpuTimings`, with the statistics; `RenderThread::setCountersEnabled`, `takeCounters`.
5. UI: `CountersWindow`; the menu entry. `main.cpp`: `dump-counters PATH`.

## Verification

- `dump-counters` on Sponza at the courtyard camera:
  - `GeometryPass` has non-zero vertex, primitive and fragment-shader counts; its `iaPrimitives` equals the sum of the triangle counts of the camera
    draws in the `drawCommands` capture (the draw log)
  - `InstanceCull` and `InstanceCullScatter` have compute invocations of the instance count rounded up to the workgroup size (407 → 512)
  - `LightingPass` fragment invocations equal the pixel count (2560 × 1440)
- Zones: `GeometryPass` has nested `camera.single` and `camera.double`; `ShadowPass` has one per cascade and bucket.
- `cull off`: `GeometryPass` primitives rise to the whole scene's triangles.
- Screenshots unchanged; validation clean with the counters on and off.

## Results

Sponza, courtyard camera, frame 149 (`150 dump-counters PATH`), with `window counters on` also checked:

- `GeometryPass`: 447,895 input primitives, equal to `primitives.r0` in the same frame's `drawCounts` capture; 116,978 leave clipping (backfaces and
  off-screen parts removed); 1,310,163 fragment invocations (0.36 per pixel: most of this view is sky).
- `ShadowPass`: 6,702,395 input primitives, the exact sum of the three cascade regions (846,869 + 2,334,039 + 3,521,487); no fragment invocations
  (depth only).
- `LightingPass`: 3,686,400 fragment invocations, 2560 × 1440. `AAPass`: 3,686,400 compute invocations.
- `InstanceCull` and `InstanceCullScatter`: 512 compute invocations (407 instances in workgroups of 256); `InstanceCullScan`: 256 (one workgroup).
- Zones: `GeometryPass` has `camera.single` and `camera.double`; `ShadowPass` has `cascadeN.single/double` for each of the 3 cascades.
- `cull off`: `GeometryPass` input primitives rise to 3,747,022 and `ShadowPass` to 11,241,066 (the whole scene, three times).
- Vertex-shader invocations equal input vertices on this driver: it reports no post-transform cache reuse for indirect draws.
- The six screenshots are byte-identical to the baseline; every run is validation clean, counters on and off.
- Fix found on the way: `pipelineStatistics` in `RhiDeviceLimits` was set before the struct was assigned wholesale; it now sits in the initializer.
