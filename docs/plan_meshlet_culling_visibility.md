# Meshlet culling: counted, occluded at a still camera, and seen

**Status. Landed.** Decisions 1–5 locked 2026-10-22, by the agent on the user's instruction ("go fix all of it, your decisions"); the
reasons are kept so each can be revisited. Follows [plan_meshlets.md](plan_meshlets.md) and [plan_occlusion_culling.md](plan_occlusion_culling.md).

## Current state

The task shaders cull meshlets (`meshlet.task`: frustum, normal cone, and the depth pyramid in occlusion's late phase), and the geometry and
shadow passes count them. Measured on Sponza from `/root/PhysCamera001` (debug build), the tools don't show this:

- **The main numbers are per instance.** The Culling window's drawn and culled triangles, the status bar's triangles, the Engine Map's
  frame graph line and the `culling` record's `camera.triangles`, `cascades[].triangles` and `regions[].triangles` all sum the triangles of
  whole instances that passed instance culling: 2.19 M for the camera, while the geometry pass draws 1.47 M (cascade 1: 3.75 M against 3.54 M).
- **Meshlet occlusion stops when the camera does.** Only the late phase (instances not drawn last frame) tests meshlets against the pyramid.
  At a still camera it is empty, so `meshlets.occluded` is 0 and the first phase's meshlets are never tested against depth.
- **Counts disagree.** The window's drawn instances (246) don't subtract occluded ones, the record's (240) do; the Engine Map subtracts
  neither occluded instances nor LOD levels not drawn. The GPU Scene window's Culling tab has no row for occluded instances (code `C`) and
  leaves them out of its culled list.
- **No meshlet is shown culled.** "Show culled" outlines instance boxes; `debugview meshlet` colours only meshlets that were drawn.
- **The work-item buffer grows far past need.** A frame that overflows the meshlet work items is followed by the frames already in flight
  reporting the same overflow, each doubling the buffer: Sponza went 131,072 → 4,194,304 items (32 MiB per frame slot) for about 180 k used.

## Scope

In:
- The counts: instances, meshlets and triangles at each step (scene, after instance culling, drawn), consistent across the Culling window,
  the status bar, the Engine Map and the `culling` record.
- Meshlet occlusion against the frame's final depth in the geometry pass (decision 2).
- A meshlet culling debug view (decision 3).
- The GPU Scene window's occluded instances.
- The work-item growth bug.
- `src/renderer/README.md` updates.

Out:
- Per-meshlet history (two-phase at meshlet granularity): Deferred.
- Occlusion for the shadow cascades: deferred by the occlusion plan, unchanged.
- The `render` record's per-pass `primitives`: instance-level by construction (counted per region at the cull), left and labelled.

## Decisions

### 1. What "drawn" means

**Locked**: the step order, everywhere. Scene → after instance culling (frustum, occlusion, render LOD) → drawn (after meshlet culling). The
headline "drawn" (window, status bar, Engine Map, record `triangles`) is what the mesh passes' task shaders kept; the instance-level numbers
stay, named `instanceTriangles`. Instances drawn is the camera's draws over both phases (`earlyDrawn + lateDrawn`), the number of instances
whose meshlets went to the task shaders. An instance occluded after being drawn in the first phase counts as drawn and as occluded: both
happened. Alternative: swap the numbers and drop the instance level; rejected, because then what each step removes can't be read.

### 2. Meshlet occlusion at a still camera

**Locked: the final pyramid in the geometry pass.** With occlusion on, `HzbBuildFinal` rebuilds the pyramid from the prepass's complete depth
(both phases), and the geometry pass tests every meshlet of both phases against it. Safe under the geometry pass's `Equal` test: a meshlet
that wrote any depth has that depth at a covered texel, so the farthest depth over its rectangle is no nearer than its own nearest point and
the strict test keeps it; a meshlet the test hides has no fragment that could equal the stored depth. The geometry pass then shades only
meshlets that show. The prepass still draws every first-phase meshlet (it makes the occluders). The debug views draw as the geometry pass does.

Alternatives: per-meshlet history (a bit per meshlet, both phases per meshlet) removes the prepass's work too, at the cost of a buffer sized by
the meshlets drawn and a larger change to culling; deferred until the prepass's GPU time shows it. Leaving it as is was rejected: the counter
reads 0 and the geometry pass draws hidden meshlets.

### 3. Seeing meshlet culling

**Locked: `debugview meshletcull`.** The task shader runs every test and keeps every meshlet (`meshletFlagClassify`), handing the result to the
mesh shader: green kept, red frustum, blue cone, magenta hidden by depth. The view draws into a depth buffer of its own (the culled meshlets
aren't in the frame's depth), then an x-ray: the occluded meshlets again where they lie behind what was drawn (`Less`, reversed-Z), so hidden
meshlets show through their occluders. Frustum and cone results show from elsewhere with "Freeze frustum" (live, frustum-culled meshlets are
off screen and cone-culled ones face away). Instances culled whole aren't drawn: "Show culled" outlines those.

### 4. Where the counts live

**Locked**: `CullResult` gains `cameraDrawn`, `cameraInstanceTriangles`, `cascadeInstanceTriangles`, `sceneMeshlets` and `regionMeshlets`;
`cameraTriangles` and `cascadeTriangles` become the meshlet-culled triangles. The Culling window takes the `CullResult` itself (the view's
`latestCull`, alive for the whole loop) instead of a dozen copied fields.

### 5. The work-item growth

**Locked**: each frame slot records the work-item capacity its frame was built with; a readback from a frame built with less than the current
capacity reports no overflow (it was answered already).

## Steps

1. **Growth.** `DrawLists`: `Slot::builtWorkCapacity`, set in `writeParams`; `parseReadback` zeroes the overflow of a slot built with less.
2. **Counts.** `CullResult` fields (decision 4), `Renderer::cullResult`; `cullingRecord` (`camera.drawn`, `camera.instanceTriangles`,
   `camera.triangles` from the meshlets, the same per cascade, `regions[].meshlets`, `regions[].instanceTriangles`, `sceneMeshlets`); the
   status bar; the Engine Map's frame graph, meshlet cull and occlusion lines.
3. **Culling window.** `CullingWindowInputs` holds `const CullResult&`; the table is the step order (decision 1); instances removed by
   frustum, occlusion and render LOD, meshlets by frustum, cone and depth; a "Colour meshlets by culling" switch for the debug view.
4. **GPU Scene window.** The occluded row and occluded instances in the culled list.
5. **Final pyramid.** `DepthPyramid::addBuildPass(fg, depth, pyramid, name)`; the renderer adds `HzbBuildFinal` after `DepthPrepassLate`;
   `GeometryPass` and `DebugViewPass` pass `meshletFlagOcclusion` in both phases.
6. **Debug view.** `DebugView::MeshletCull` (`meshletcull`); `meshlet.task` and `meshlet.glsl`: `meshletFlagClassify`, `meshletFlagHiddenOnly`,
   the reason in the payload; `debugview.mesh`, `debugview.frag`, `debugview.comp`; `DebugViewPass`: the classify and x-ray pipelines and the
   view's own depth; the Debug View window's legend and readout.
7. **README and Engine Map** descriptions.

## Visibility

- **Records**: `culling` changes as in step 2; `render`'s passes gain `HzbBuildFinal`.
- **Verbs and RPC**: `debugview meshletcull` (the existing verb and method take the new name); no new method.
- **Windows**: the Culling window's step table and debug-view switch; the GPU Scene window's occluded instances; the Debug View window's
  legend for `meshletcull`; `capture HzbBuildFinal hzb` shows the final pyramid.
- **Engine Map**: the occlusion node's description and line name the final pyramid; the meshlet cull node's line counts by depth; the frame
  graph's line is the drawn triangles.
- **Trace**: `MeshletWorkGrown` stays; it should fire at most once per real overflow.

## Verification

- **Growth**: Sponza's start-up trace has at most one `MeshletWorkGrown`, and the `memory` record's `cull.workitems` is under 4 MiB a slot.
- **Counts**: on Sponza at `/root/PhysCamera001` the record's `camera.triangles` equals `camera.meshlets.triangles` and is below
  `camera.instanceTriangles`; `camera.drawn` equals `occlusion.drawnFirst + drawnSecond`; the window shows the same numbers (by eye, handed off).
- **Occlusion at a still camera**: `camera.meshlets.occluded` > 0 at frame 200+ with occlusion on, and 0 with it off.
- **Same image**: screenshots with occlusion on and off byte-identical at a still camera (both with the prepass); occlusion on stays
  byte-identical to the commit before this change.
- **Debug view**: `--view` is lit-only, so `debugview meshletcull` by script and a screenshot: green dominant, magenta x-ray present with
  occlusion on; with the frustum frozen and the camera moved, red and blue present.
- **No validation messages** on Sponza and three_cubes (`--fail-on-validation`).

## Results

Sponza from `/root/PhysCamera001`, debug build, frame 150 at a still camera:

- **Growth**: one `MeshletWorkGrown` at start-up (131,072 → 262,144 items) where there were five (to 4,194,304).
- **Counts**: `camera.drawn` 240 = `drawnFirst` 240 + `drawnSecond` 0; `camera.triangles` 948,949 = `camera.meshlets.triangles`, below
  `camera.instanceTriangles` 2,155,065; `sceneMeshlets` 41,012. Cascade 1: 3,541,197 drawn of 3,747,022 kept by instance culling.
- **Occlusion at a still camera**: 5,290 meshlets hidden by depth (0 before this change); the geometry pass's triangles 1.47 M → 0.95 M.
  Occlusion off: 0 hidden, 1,465,023 triangles.
- **Same image**: occlusion on and off (prepass on in both) byte-identical. The passes run `DepthPrepass`, `HzbBuild`, `DepthPrepassLate`,
  `HzbBuildFinal`, `GeometryPass`.
- **Debug view**: live, green with magenta x-ray of hidden banners and lanterns behind the columns; frozen and moved aside, the meshlets
  outside the frozen frustum red. Cone-culled (blue) meshlets weren't framed in a screenshot.
- **No validation messages** on Sponza (every run above) and three_cubes (`--fail-on-validation`, with `meshletcull` on).
- Not measured: GPU time of `HzbBuildFinal` and the geometry pass in a release build; the windows by eye (handed off).

## Deferred / follow-ups

- **Per-meshlet history** (decision 2). Trigger: the prepass's GPU zones a large share of the frame at the test world's viewpoints.
- **Meshlet triangles per pass in the `render` record.** Trigger: a need to read them per bucket; per view they are in `culling`.
