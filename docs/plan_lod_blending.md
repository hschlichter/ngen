# LOD blending: both heuristics, distance first, levels cross-faded over the authored band

**Status. Draft.** Decisions 1, 2 and 6 locked 2026-10-25.

## Current state

A render-LOD group ([plan_render_lod.md](plan_render_lod.md), `src/renderer/README.md` "Render LOD") switches level in one frame:
`shaders/lodselect.comp` picks one level per group (finer as soon as the screen size passes a threshold, coarser once it is 20% below it), and
`shaders/instancecull.comp` drops every instance of the other levels. Nothing blends. The packer keeps a heuristic's `blendThresholds`
(`src/asset/pack/usd/usdpacker.cpp`), but the composer doesn't read them: `LodRoot` (`src/world/world.h`) has `thresholds` only.

The engine reads one heuristic per root: the first `LODScreenSizeHeuristic` in the imaging domain with usable bounds. A
`LODDistanceHeuristic` is never read. The test world authors both on every region and tile, in either order (`R_0_1_full.usda` lists the
distance one first), and so does the test asset `assets/streamtest-world/world/assets/lodtest/oak_lod8.usda` (screen size first): 8 levels
(15,998 to 48 triangles), screen-size thresholds `[0.25 … 0.0032]` with blend thresholds at 85% of each, distance thresholds `[49.3 … 429.4]`
with blend thresholds `[53.4 … 465.6]`.

UsdLod (`external/openusd/pxr/usd/usdLod/schema.usda`, `distanceHeuristicQuery.*`, `screenSizeHeuristicQuery.*`):
- **Distance**: from the viewpoint to the root's `center`, or, when `boundingVolume` names a valid extent, to the nearest point of that box,
  both in the root's local space (the viewpoint is taken into it, the point back out): 0 inside the box. Thresholds ascend.
- **Screen size**: the extent's projected sphere (or box) as a fraction of the view. Thresholds descend.
- **Blending**: between `thresholds[k]` and `blendThresholds[k]` (beyond it: larger for distance, smaller for screen size) the query returns
  a fractional level, `k` plus the blend amount, linear; a missing value means "a discontinuous jump". Blending is the authored alternative
  to hysteresis. A renderer uses the first heuristic in `lod:heuristics` it understands.

## Scope

In:
- **Both heuristics**, for streaming roots (the view's evaluation, on the CPU) and render-LOD groups (the GPU pick): the World keeps every
  heuristic a root authors, and the view picks one per root by a view-wide preference, distance first by default (decision 6).
- The composer reads `blendThresholds`, clamped as UsdLod says; groups carry the chosen heuristic's thresholds and bands.
- The GPU pick computes the fractional level where a band is authored: two levels and a blend amount per group.
- The two levels cross-fade with complementary dither in the depth prepass, the geometry pass and the shadow cascades.
- Where a band is authored it replaces the hysteresis at that boundary; where none is, the switch and the hysteresis stay as now.
- The preference as a method and a verb; records, window counts and the Engine Map for heuristics and fades.
- Verified on the oak and the layered scene: the core behaviour first, before scaling (2026-10-25). The test world is being re-authored with
  higher-fidelity LODs and isn't measured here (Deferred).

Out:
- Blending streaming levels (tiles and regions switching): the same mechanism could fade them, but their levels are separate placements
  loaded and released; Deferred.
- Time-based fades for assets without blend thresholds (decision 2).
- Heuristics outside the imaging domain (the test world's `physics_distance`) and of other types: listed, not read.
- Temporal anti-aliasing to hide the dither pattern.
- The LOD tool's inspector and tuning: [plan_lod_tool.md](plan_lod_tool.md), which builds on this.

## Decisions

### 1. Technique — locked: A

- **A. Dithered cross-fade.** While a group fades, both levels draw; each pixel keeps one of them by a screen-space threshold pattern
  compared with the blend amount, the two levels' tests complementary, so every pixel is covered once. Opaque, so no sorting; depth and
  shadows work as for any cutout. What most engines ship for LOD transitions.
- **B. Alpha blending.** Needs transparency sorting and a forward path the renderer doesn't have.
- **C. Geomorphing.** Needs vertex correspondence between levels; authored levels have none.

Locked (2026-10-25): it fits the deferred, opaque renderer and its cutout path.

### 2. What drives the fade — locked: A

- **A. The authored band only.** The blend amount is UsdLod's, linear between `thresholds[k]` and `blendThresholds[k]`. No state: a camera
  parked in the band draws both levels (bounded: a group fades across at most one boundary). An asset without blend thresholds pops, as now.
- **B. A fixed time** after the level changes, for every group: a fade state per group, and it ignores what is authored.
- **C. A, and B where nothing is authored.**

Locked (2026-10-25): authoring drives the fade. C stays Deferred with its trigger.

### 3. How fading instances draw

The renderer's opaque pipelines don't discard, so they keep early depth testing; the cutout pipelines discard (`shaders/cutout.glsl`).
- **A. Fading instances draw in the cutout buckets** of their sidedness: the cull pass puts an instance of a fading group into the cutout
  bucket for the frame, and `cutout.glsl` adds the dither test. No new pipelines. Region capacities are sized on the CPU from bucket counts
  (`DrawLists::countInstance`): a render-LOD instance counts in its cutout bucket as well as its own, so any instance can fade in a frame.
- **B. A dither discard in the opaque pipelines**, behind a per-instance flag. Every opaque draw loses early depth testing.
- **C. Fade buckets and pipelines of their own.** The same as A with more pipelines.

I lean **A**. Its cost is command capacity for every render-LOD instance in the cutout regions (a 20-byte command each; on the test world
some 800,000 render-LOD instances, so about 16 MB per frame slot for the camera, and up to four times that again for the shadow cascades'
whole-mesh draws); measured in step 6, and a reason to revisit if it shows.

### 4. The pattern

- **A. A 4×4 ordered (Bayer) matrix** in screen space: no texture, deterministic, so screenshots stay byte-identical run to run.
- **B. Blue noise** from a texture: less visible structure, one more texture and binding.

I lean **A**, and accept the visible pattern while a fade lasts (no TAA to resolve it).

### 5. Residency

A group fades only when both levels' meshes are in the pool; otherwise it draws as now (the wanted level, else the fallback), so a fade never
shows a hole. Locked unless pushed back on: it follows from the existing fallback rule.

### 6. Which heuristic a root uses — locked: both, distance first

- **A. Authored order**, as UsdLod has a renderer do it: the first heuristic the engine reads. The test world's content lists them in either
  order, so the oak would go by screen size and some tiles by distance.
- **B. A view-wide preference**: distance first (the default), screen size first, or authored order; a root without a usable heuristic of the
  preferred kind uses the other, and one with neither keeps its default level (as now).

Locked (2026-10-25): B, distance first. A distance in metres is easier to read and reason about than a fraction of the view; it doesn't
follow a zoom (field of view), which screen size does, and the preference is there to compare. The default changes the levels the test world
draws today (screen size); it is measured again once its re-authored LODs land, not in this plan.

What a distance heuristic needs, as USD computes it: the root's transform and its inverse (the viewpoint into the root's space, the nearest
point back out), the `center`, and the `boundingVolume`'s extent when valid. Thresholds are in scene units: the view compares them with the
distance in metres divided by `metersPerUnit`. Without a band, the hysteresis for distance is the screen size's 20% expressed as a distance
(size goes as 1/d², so coarser once the distance is 12% beyond the threshold).

## Steps

1. **Every heuristic in the World.** `LodRoot` keeps `std::vector<LodHeuristic> heuristics`: kind (`Distance`, `ScreenSize`, or one the
   engine doesn't read, with its type), domain, thresholds, blend thresholds, the screen size's extent and projection, the distance's center
   and bounding-volume extent, and why it isn't usable when it isn't (not the imaging domain, an empty or non-finite extent, thresholds not
   ascending or descending as their kind requires). A blend threshold outside its boundary (between `thresholds[k]` and the next) is dropped
   with a composition message, one per heuristic, and a dropped or missing one is `NaN`: no band there. The current `heuristic`,
   `heuristicChosen`, `heuristicType`, `thresholds`, `extent`, `projection` fields go; `ngen-worldcheck`/`ngen-usdcheck` print every
   heuristic, so parity covers them. Shared with [plan_lod_tool.md](plan_lod_tool.md), which shows them.
2. **The preference.** `ViewScene` picks the heuristic per root by `lodPreference` (`Distance` by default, `ScreenSize`, `Authored`):
   `chosenHeuristic(const LodRoot&) -> int32_t`. The streaming evaluation (`evaluateLevels`, `levelFor`) measures by the chosen kind; the
   distance from the posted camera. Method `view.lod.heuristic {prefer}` beside `on` and `unloadDelay`, verb `lod-prefer
   distance|screen|authored`; a change re-evaluates every root and rebuilds every group's record (`refreshForcedLevels`'s path).
3. **Groups carry the chosen heuristic.** `RenderLodGroup` and `GpuLodGroup` (`lodgroups.h`) gain the kind, the root's transform and inverse
   (3×4 each), the center, the bounding-volume extent, and `blendThresholds`; the screen size's sphere stays. The entry grows from 64 to 224
   bytes (`static_assert`); the view fills it where it places the group (`placement.cpp`), and a preference change refreshes every group.
4. **The fractional pick.** `lodselect.comp` measures by the group's kind (the distance as `ComputeDistance` does; push constants gain
   `metersPerUnit`); the wanted level as now, but at a boundary with a band the hysteresis is the band (finer on one side of
   `thresholds[k]`, coarser beyond `blendThresholds[k]`), and inside the band the group fades: the state gains the second level and the
   blend amount (`states` from `uvec2` to `uvec4`: serial, wanted | drawn << 8 | fade level << 16, the amount as float bits). A forced level
   never fades. Residency as decision 5. The view's `levelFor` takes the same band as its hysteresis, so a streaming root and a group with the
   same data want the same level. The counters gain `fading`.
5. **Drawing a fade.** `instancecull.comp`: an instance of the drawn level or the fade level is drawn; while its group fades it goes to the
   cutout bucket of its sidedness (decision 3). `DrawLists::countInstance` counts render-LOD instances in the cutout bucket too. The vertex
   stages of the cutout pipelines (`gbuffer.vert` for the cutout fragment, `depthcutout.vert`, `shadowcutout.vert`) read the group's state
   (`lodStates`, bound to their set) and pass the instance's coverage (the amount, or one minus it, and which side) flat to the fragment;
   `cutout.glsl` discards a pixel whose Bayer value is outside its side's coverage. An instance of a group not fading passes coverage 1 and
   draws as before.
6. **Measure.** The oak alone, by distance and by screen size: draws, cutout region use, frame and GPU time with and without a fade in view.
   Decision 3's command capacity is weighed on a large scene once the test world returns (Deferred).
7. **Visibility and docs.** Below; `src/renderer/README.md` "Render LOD" (both kinds, the band, the fade, the buckets), `src/world/README.md`
   (every heuristic kept, the band read), `src/view/README.md` (the preference, the heuristic's band).

## Visibility

- **Method and verb**: `view.lod.heuristic {prefer}`, `lod-prefer` (step 2).
- **`lod` and `streaming` records**: per root every heuristic (kind, domain, thresholds, bands, usable or why not) and the one used; the
  `streaming` record's `screenSize` becomes `measure` with its kind (metres or a fraction of the view); `why` says the heuristic used and a
  boundary with a band; the preference in force.
- **`renderlod` record and `view.renderlod {path}`**: per group its kind, measure, blend thresholds, fade level and blend amount; totals
  `fading` and groups by kind; the record's GPU-vs-heuristic check measures by the group's kind and compares the fractional level.
- **LOD window**: the preference beside the heuristic toggle; the Render LOD section counts groups fading. The per-root inspector is
  [plan_lod_tool.md](plan_lod_tool.md)'s.
- **Culling window**: cutout region use, with the fading instances counted apart (`counterFading`).
- **Engine Map**: LOD selection and LOD select name both kinds and the preference; LOD select's description gains the fade; Culling's says a
  fading instance draws in the cutout buckets.
- **Trace**: `LevelsWanted` says the preference; `LodPreferenceChanged` when it changes. A dropped blend threshold is a `CompositionMessage`.
  Nothing per frame: a fade is data, in the records.

## Verification

- **Parity**: `ngen-worldcheck` and `ngen-usdcheck` identical on the oak and the layered scene, every heuristic printed.
- **The pick against USD, both kinds**: a script walks the camera away from the oak along one line through every boundary, `record renderlod`
  inside each band and outside it, once with `lod-prefer distance` and once with `lod-prefer screen`. At every record the group's fractional
  level (drawn plus amount) equals `UsdLodDistanceHeuristicQuery`'s or `UsdLodScreenSizeHeuristicQuery`'s result for the recorded camera
  within 1e-4 (computed with the Python OpenUSD build from the recorded eye and the root's transform).
- **The preference**: on the oak, `lod-prefer authored` uses its first heuristic (screen size; the records name it); on the layered scene,
  whose streaming root authors only a screen-size heuristic, every preference falls back to it and the LOD script gives the same levels as
  the commit before.
- **Coverage**: screenshots of the oak mid-band at amounts 0.25, 0.5 and 0.75 show no hole: the id target (`gbuffer.instance`) has a
  covered pixel wherever either level alone covers one, and the share of pixels from the finer level is within 5% of the amount over the
  overlap.
- **No fade outside a band**: outside the bands, and for groups without one, nothing fades and draws equal the same preference's levels; the
  eight baselines byte-identical.
- **Shadows**: a mid-band screenshot with the sun shows the dithered shadow (both levels' casters), no shadow missing.
- **Records and windows** from Visibility show the oak's heuristic, band, fade level and amount, and `fading` 1 while mid-band, 0 outside.

## Open questions

- A camera that stops mid-band shows the dither pattern until it moves. Acceptable for a first version, or does it want a time limit after
  which the nearer level wins (state per group)?
- Should the dither pattern be offset per instance, so two overlapping fading trees don't share the same pixels? Not needed for the oak;
  maybe for forests.

## Deferred / follow-ups

- **The test world under distance and fades.** Trigger: its re-authored, higher-fidelity LODs land; then its viewpoints and the 27-camera
  flight are measured by distance (levels, draws, pass times, memory) and decision 3's command capacity is weighed at scale.

- **Streaming levels fade.** Trigger: tile switches judged too abrupt on the test world; needs both levels' placements drawn during the
  switch, which step 5 of streaming v2 already holds loaded.
- **Time-based fades where nothing is authored** (decision 2 C). Trigger: content that can't author bands and pops visibly.
- **Blue noise or TAA** to hide the pattern (decision 4). Trigger: TAA lands, or the pattern is judged too visible.
- **Other domains** (`physics`) and heuristic types. Trigger: a system that consumes them.
