# Indirect draws

**Status. Landed.**

Stage 4 of [plan_gpu_driven.md](plan_gpu_driven.md).

## Current state

After stages 1 to 3, a draw depends only on data the GPU can index:

- geometry from the pool (`firstIndex`, `vertexOffset`)
- the instance record at `gl_InstanceIndex` (`firstInstance = m`)
- the material through the instance record

The geometry pass, the depth prepass and the shadow pass still loop on the CPU. They issue one `drawIndexed` per visible instance, filtered by
`RenderSnapshot::visible` (camera) and `shadowVisible[c]` (per cascade), in two pipeline buckets (single-sided, double-sided). Sponza at the courtyard
camera: 65 geometry draws, 204 shadow draws over three cascades.

The render debugger depends on the CPU issuing each draw:

- `FrameGraphContext::beginDraw` logs one `FgDrawRecord` per draw.
- "Time draws" (`FgDrawTimingRequest`, plumbed from the Render Debug window through `RenderThread` to `FrameGraph`) wraps a window of draws in GPU
  zones.
- The geometry and shadow passes open a `LargeDraw` zone around draws of 100k+ indices.
- `RhiCommandStats::draws` and `primitives` are counted per `drawIndexed` call.

The RHI has no indirect commands, `RhiBufferUsage::Indirect` or `RhiBufferState::IndirectRead`. The Vulkan device enables none of
`multiDrawIndirect`, `drawIndirectFirstInstance` or `drawIndirectCount`; RADV reports all three.

## Scope

**In**

- **RHI:**
  - `drawIndexedIndirect` and `drawIndexedIndirectCount`.
  - `RhiDrawIndexedIndirectCommand`, `RhiBufferUsage::Indirect`, `RhiBufferState::IndirectRead`, and a new `indirectDraws` counter in
    `RhiCommandStats`.
  - The three features enabled and required.
  - `ngen-example-indirect`.
- **Frame graph:** `FgAccessFlags::IndirectRead`, mapping to `RhiBufferState::IndirectRead`.
- **Command lists:** the render thread turns the CPU visibility masks into indirect commands per view and bucket, each frame.
- **Draw passes:** the geometry, prepass and shadow passes issue one `drawIndexedIndirectCount` per bucket per view.
- **Debug tools** kept correct at pass granularity (the umbrella decision). See decision 4.

**Out**

- GPU culling. That is stage 5, which replaces the CPU producer of the same buffers.
- Rebuilding per-draw debugging on GPU data (the umbrella's debugging and introspection pass after stage 5).
- Indirect dispatch. Nothing dispatches indirectly until culling does, and it's added with its consumer.
- Sorting draws differently. The order within a bucket stays instance order, so depth ties resolve as today.

## Decisions

All five recommendations were confirmed and are locked. On features: Vulkan 1.2 is the project baseline (possibly higher later), so
`drawIndirectCount` comes from `VkPhysicalDeviceVulkan12Features` with no extension path.

1. **Where the commands live.**
   - (a) A host-visible command buffer and count buffer per frame slot, written by the render thread and read by the GPU directly. Both are imported
     into the graph and read with `IndirectRead`. The slot's fence guards them, like the UBO and the instance staging.
   - (b) Device-local buffers filled through staging and a graph copy pass, like the instance buffer.

   I lean (a). The lists change every frame, so a copy buys nothing but a pass, and stage 5 replaces them with GPU-written transient buffers anyway.
   Declaring them `IndirectRead` in the graph now means stage 5 only swaps the producer; the consumers don't change.
2. **Count source: `drawIndexedIndirectCount` now, or `drawIndexedIndirect` with a CPU count?** I lean the Count variant, reading a CPU-written
   count buffer. Stage 5's culling writes the count on the GPU, so using it now keeps stage 5 a producer-only change. The RHI gets both calls, and
   the example exercises both.
3. **Command layout.**
   - One command buffer per slot, split into regions per view and bucket: camera single-sided, camera double-sided, and cascade 0..3 × two buckets.
     Each region is sized to the instance count.
   - One count buffer holding one `uint` per region.
   - The geometry pass and the depth prepass share the camera regions. They draw the same visible set, and the position and vertex pools share
     `vertexOffset`.
   - Shadow regions hold whole-mesh commands on `primFirst` instances, as the shadow loop draws today.
4. **Debug tools under indirect draws.** Following the umbrella decision (correct at pass granularity, per-draw detail rebuilt later):
   - **Draw log: kept.** The render thread knows every command in this stage, so `beginDraw` records stay exact. They're logged from the CPU list
     rather than around each draw call. Stage 5 makes them approximate until the debugging pass.
   - **Per-pass `draws` and `primitives`: kept.** The RHI can't see inside an indirect call, so the pass reports its commands and primitives from the
     CPU list, through a new `FrameGraphContext::addIndirectStats(draws, primitives)` that folds into the pass's `RhiCommandStats`. The RHI counts
     the calls in `indirectDraws`.
   - **"Time draws" and `LargeDraw` zones: removed.** A GPU zone can't open inside a multi-draw call. I lean deleting the machinery:
     `FgDrawTimingRequest`, its `RenderThread` and `FrameGraph` plumbing, the Render Debug checkbox and the zone joining in `readGpuTimings`.
     Keeping it half-working would contradict "GPU-driven only". The debugging pass decides what replaces it, for example per-bucket zones or GPU
     counters.
5. **Features: required, no fallback.** `multiDrawIndirect` and `drawIndirectFirstInstance` are `VkPhysicalDeviceFeatures` members.
   `drawIndirectCount` is a `VkPhysicalDeviceVulkan12Features` member, so the device must be Vulkan 1.2 or newer. `RhiDeviceVulkan::init` fails with
   a message if any is missing, as the umbrella decided.

## Steps

1. **RHI** (`rhitypes.h`, `rhicommandbuffer.h`, Vulkan backend).
   ```cpp
   // Layout matches VkDrawIndexedIndirectCommand / D3D12_DRAW_INDEXED_ARGUMENTS.
   struct RhiDrawIndexedIndirectCommand {
       uint32_t indexCount = 0;
       uint32_t instanceCount = 0;
       uint32_t firstIndex = 0;
       int32_t vertexOffset = 0;
       uint32_t firstInstance = 0;
   };

   virtual auto drawIndexedIndirect(RhiBuffer* commands, uint64_t offset, uint32_t drawCount) -> void = 0;
   virtual auto drawIndexedIndirectCount(RhiBuffer* commands, uint64_t offset, RhiBuffer* count, uint64_t countOffset, uint32_t maxDrawCount) -> void = 0;
   ```
   - `RhiBufferUsage::Indirect` and `RhiBufferState::IndirectRead` (stage `DRAW_INDIRECT`, access `INDIRECT_COMMAND_READ`).
   - `RhiCommandStats::indirectDraws`.
   - Feature enables and the device-version check in `init`.
   - `src/rhi/README.md`: an examples row, and the known-gaps line "no indirect dispatch" narrowed.
2. **`ngen-example-indirect`** (`src/rhi/examples/indirect.cpp`).
   - Eight quads in one vertex/index buffer, each command with `firstInstance = i` selecting a colour from a storage buffer.
   - Frame: one `drawIndexedIndirect` with 4 commands, then one `drawIndexedIndirectCount` with `maxDrawCount` 4 and a count buffer holding 3.
   - `--check`: quads 0..3 and 4..6 drawn in their colours, quad 7 is clear colour (proving the count was honoured), `indirectDraws == 2`.
3. **Frame graph.** `FgAccessFlags::IndirectRead`, mapped in `accessToBufferState`. `FrameGraphContext::addIndirectStats(uint32_t draws,
   uint64_t primitives)` adds to the executing pass's stats delta. `beginDraw`/`endDraw` became `logDraw`, since there's no timing zone left to
   close.
4. **Command lists** (new `src/renderer/drawlists.h/.cpp`, owned by `Renderer`).
   - Per frame slot: a `CpuToGpu` mapped command buffer (`Indirect`) of `regions × instanceCapacity` commands, and a count buffer. Both grow with
     the instance buffer.
   - `buildDrawLists(instances, visible, shadowVisible, cascadeCount, const GpuScene&, slot)` writes the regions and counts. It keeps a CPU mirror
     (the region's commands plus instance indices) for the draw log and stats.
   - `import(FrameGraph&, slot) -> Handles { commands, counts }`.
   - `report(ctx, region, instances)` logs the region's commands to the draw log and calls `addIndirectStats`. The passes call it after each
     indirect call.
5. **Passes.** Each pass declares `read(commands, IndirectRead)` and `read(counts, IndirectRead)`.
   - Per bucket: bind the pipeline, the set and the pool, then call `drawIndexedIndirectCount(commands, regionOffset, counts, regionIndex * 4,
     regionCapacity)`.
   - The CPU loops, the `visible` masks in the pass signatures, and the `LargeDraw` zones go.
   - Draw-log records and `addIndirectStats` come from the mirror.
6. **Remove per-draw timing.** `FgDrawTimingRequest`, `FrameGraph::setDrawTiming`, the timed branch in `beginDraw`, `RenderThread`'s timing mutex
   and setter, `Renderer::setDrawTiming`, the Render Debug "Time draws" UI, the zone joining in `readGpuTimings`, `largeDrawIndexCount`.
7. **Observation.** `RenderStats` gains `indirect_draws` (calls) and `draw_commands` (commands written this frame).

## Verification

Baseline: the stage 3 screenshots (byte-identical to the pre-stage-1 ones) and the stage 3 dump.

- `ngen-example-indirect --check --validation` exits 0. **Verified**:
  - Quads 0..6 are drawn in their `firstInstance` colours.
  - Quad 7, past the GPU count of 3, is clear colour.
  - `indirectDraws` is 2 and direct `draws` is 0.
  - The other 14 examples pass.
- Screenshots byte-identical, prepass off and on. **Verified**: all six pairs identical.
- `--fail-on-validation` exits 0. **Verified** on all runs.
- Sponza at the courtyard camera, compared with stage 3. **Verified**:
  - Per-pass `draws` and `primitives` are identical (`ShadowPass` 204 and 6,702,395; `GeometryPass` 65 and 447,895).
  - `indirectDraws` is 3 for `ShadowPass` (one bucket per cascade here) and 1 for `GeometryPass`.
  - The draw log is identical entry for entry: pass, index, instance, mesh, prim, index range.
- `RenderStats`. **Verified**: `draw_commands` is 269, the sum of the per-pass draws. `indirect_draws` is 4, and the remaining direct `draws` are
  3 (lighting, blit, UI). The stage 3 total was 272 direct draws.
- `cull off`. **Verified**: `GeometryPass` has 407 draws in 1 indirect call, and `ShadowPass` 345 draws (115 meshes × 3 cascades) in 3.
- The `translate` script still moves the cube. **Verified**: the screenshot matches stage 1.
- GPU time on Sponza within noise. **Verified**: `GeometryPass` 1.28 to 1.26 ms, `ShadowPass` 3.63 to 3.63 ms.
- Render-thread record time was not measured. The stage 3 binary wasn't kept for a side-by-side profile.

## Deferred / follow-ups

- Indirect dispatch: with stage 5's culling pass.
- Per-draw timing and GPU-side draw statistics: the debugging and introspection pass after stage 5.
- Device-local command buffers: if a discrete GPU shows the host-visible reads cost something.
