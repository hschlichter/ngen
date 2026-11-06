# Occlusion culling: two-phase, against a depth pyramid

**Status. Draft.** Decisions 1–8 locked 2026-10-22, by the agent on the user's instruction ("make all the decisions"); the reasons are kept
so each can be revisited. The second half of the roadmap's vertical slice step 2, after [plan_meshlets.md](plan_meshlets.md), whose task
shaders this extends.

## Current state

- **Culling** is per instance against frusta (`src/renderer/README.md` "Culling and draw lists"), and after plan_meshlets per meshlet
  against frusta and normal cones. Nothing hidden behind something else is culled: at Caldera's airfield 106 M of 136 M triangles were
  drawn.
- **The depth prepass** is optional (`prepass on|off`, off by default) and draws the camera regions before the geometry pass, which then
  tests `Equal`.
- **Depth** is one D32 texture imported every frame, cleared each frame (reversed-Z, 0 far). No depth pyramid, no per-mip views, no
  `R32_SFLOAT` format in the RHI.
- **Visibility** buffers are per frame slot: nothing about the previous frame reaches culling.
- **The frame graph** orders a pass after the last writer of what it reads, and writers after writers; a writer is not ordered after the
  readers of the previous contents (no write-after-read edge).

## Scope

In:
- A depth pyramid (HZB) built each frame from the camera's depth.
- Two-phase occlusion of the camera's instances: phase 1 draws what was visible last frame, the pyramid is built, phase 2 tests the rest
  against it and draws what shows; a persistent per-instance history.
- Meshlet occlusion in phase 2's task shaders.
- The depth prepass as the place both phases draw, the geometry pass drawing both lists.
- The frame graph's write-after-read edges.
- Records, verbs, the Culling window, a pyramid view, the Engine Map.

Out:
- Occlusion for the shadow cascades: their views see what the camera doesn't (Deferred).
- Per-meshlet history (meshlet occlusion in phase 1): Deferred, with its trigger.
- Occluder geometry or software rasterization: the previous frame's visible set is the occluder.

## Decisions

### 1. Where the phases draw

- **A. In the depth prepass**: phase 1 and phase 2 draw depth only; the geometry pass then draws both lists with the `Equal` test, so
  every pixel is shaded once.
- **B. In the geometry pass**: two geometry passes, the second loading the G-buffer; no prepass.

**Locked: A.** One geometry pass, which the lighting, debug view and picking code expect; the prepass exists already and shades nothing.
`occlusion on|off` is its own switch, on by default, and runs the prepass whatever the prepass switch says; `prepass` matters only with
occlusion off. (The draft had `prepass off` turn occlusion off; two switches that each turn the other off read worse than one that
implies the other, so it changed while building.)

### 2. The pyramid

**Locked**: `R32_SFLOAT`, mip 0 the depth texture's size, every mip down to 1×1, each texel the *minimum* of what it covers (reversed-Z: the
farthest depth), odd sizes folding the last row and column into the texel next to them so nothing is lost. Built by `HzbBuild`, one compute
dispatch per mip, reading mip `i − 1` and writing mip `i` as storage images. Owned by a `DepthPyramid` class, kept across frames, recreated
with the swapchain, imported into the graph each frame. Built from the frame's phase-1 depth, so it is never a frame late and needs no
reprojection.

### 3. Barriers inside the build

The graph transitions whole textures between passes; the build needs mip `i − 1`'s writes visible before mip `i` reads them.
- **A. A pass per mip, mip-level resources in the graph**: subresource handles and barriers, a large change to the graph.
- **B. One pass, the whole pyramid in `General`**: storage reads and writes both, an execution and memory barrier (`General` to `General`)
  between dispatches, recorded through the frame graph context so it shows in the frame debugger's barriers.
- **C. A single-pass downsampler** (AMD SPD): one dispatch, global atomics, no barriers; harder to read.

**Locked: B.** Per-mip storage views are the only RHI addition (`RhiDescriptorWrite::mipLevel`), and the graph keeps whole-texture state.
The in-pass barrier goes through `FrameGraphContext::textureBarrier` so the frame debugger lists it with the graph's own.

### 4. History

**Locked**: a persistent buffer, one word per instance slot (`cull.history`, not per frame slot: the last frame's is another slot's), bit 0
"visible to the camera last frame". Phase 1 draws the frustum-visible instances with the bit; phase 2 tests every frustum-visible instance
against the pyramid, writes the bit for all of them, and draws those that pass and weren't drawn in phase 1. A grown buffer starts at 0
(everything goes through phase 2 once); a slot taken by new content keeps its bit, which costs one phase-1 draw at most.

### 5. The instance test

**Locked**: the world box's eight corners projected with the live camera's view-projection; a corner at or behind the near plane makes the
instance visible; else the screen rectangle's size picks the mip where it covers at most 2×2 texels, four `texelFetch`es give the farthest
depth there, and the instance is visible when its nearest depth (the largest, reversed-Z) is at or above it. A meshlet: its sphere's
screen rectangle the same way, its nearest depth from the sphere's nearest point.

### 6. Meshlets

**Locked**: meshlet occlusion runs in phase 2's task shaders (phase 2 regions, and the geometry pass's draw of them with the same pyramid,
so both passes keep and drop the same meshlets: the geometry pass must draw at least what the prepass drew, or the `Equal` test leaves
holes). Phase 1's meshlets are frustum- and cone-culled only. Trigger for per-meshlet history: the meshlet counts at Caldera or the test
world show phase 1 drawing many occluded meshlets of large meshes.

### 7. The frozen frustum

**Locked**: occlusion is skipped while the culling frustum is frozen (`cull freeze`); the pyramid comes from the live camera and the two
don't mix. The `culling` record says so.

### 8. Regions

**Locked**: phase 2's draws are a view of their own: view 5 "camera, phase 2", four buckets, 24 regions. Phase 1's scan writes the counts of
views 0–4, phase 2's those of view 5, so the two never overwrite each other and each draw call has an offset known on the CPU.

## Steps

1. **Frame graph write-after-read edges.** `FrameGraph::compile`: each reader gets an edge to the next writer after it. Baselines unchanged.
2. **RHI.** `RhiFormat::R32_SFLOAT`; `RhiDescriptorWrite::mipLevel` (−1 the whole texture), the Vulkan backend's per-mip views created with
   a storage texture that has mips; `ngen-example-compute` or a new `ngen-example-mipchain` writes and reads mips.
3. **The pyramid.** `src/renderer/depthpyramid.{h,cpp}` (`DepthPyramid`: texture, per-mip descriptor sets, `addBuildPass`),
   `shaders/hzbbuild.comp` (mode 0: depth to mip 0, mode 1: mip to mip), `FrameGraphContext::textureBarrier`.
4. **Two phases.** `instancecull.comp`: modes for phase 1 (frustum, LOD, history picks the camera's phase-1 region) and phase 2 (the pyramid
   test, history written, view 5 regions), scan per phase; `DrawLists` the history buffer and 24 regions; `InstanceCullPass` the phase-2
   passes; `DepthPrepass` draws phase 1, then (after `HzbBuild`) phase 2; the geometry pass and the debug views draw both. The cull code
   `C` (occluded).
5. **Meshlets.** `meshlet.task`: the pyramid test for phase-2 regions.
6. **Visibility** (below), the `occlusion` verb, README updates.

## Visibility

- **Records**: `culling` gains `occlusion` (enabled, why not, instances drawn in phase 1, in phase 2, occluded, and meshlets occluded);
  `memory` lists `hzb` and `cull.history`; `status` the switch.
- **Verbs and RPC**: `occlusion on|off`, `cull view` overlay colours occluded instances apart (code `C`).
- **Windows**: the Culling window's occlusion rows and switch; the pyramid in the Frame Graph window's previews and through `capture HzbBuild
  hzb`; a `hzb` buffer view (`--view=hzb`) showing a pyramid mip (`occlusion mip=<n>`).
- **Engine Map**: "Depth pyramid" and "Occlusion (phase 2)" in the render thread's lane, edges from the prepass's depth to the pyramid and
  from the pyramid to phase 2.
- **Trace**: none per frame.

## Verification

- **Graph**: the frame order with occlusion on puts `HzbBuild` after `DepthPrepass` phase 1 and before phase 2 (the `render` record's
  passes); baselines unchanged by step 1.
- **Correctness**: Sponza and the test world screenshots with occlusion on and off byte-identical at a still camera (occlusion removes only
  what doesn't show); the same after a camera cut (`--camera` at frame 0 and a move by script), from the frame after the cut on.
- **It culls**: the `culling` record at Sponza's atrium and the test world's dense_forest and valley: occluded instances > 0, phase 2
  draws few after the first frames (the history holds), camera triangles drawn below the occlusion-off run's.
- **No validation messages** on every scene above.
- **Cost, measured and recorded** (release): `HzbBuild`, both cull phases and the prepass phases' GPU zones; the geometry pass's time with
  occlusion on and off at the test world's viewpoints.

## Deferred / follow-ups

- **Per-meshlet history** (decision 6). Trigger as stated there.
- **Cascade occlusion** (each cascade's own pyramid). Trigger: shadow pass time dominating at the test world's viewpoints.
- **Single-pass pyramid build**. Trigger: `HzbBuild` above 0.2 ms on hardware.
