# The LOD tool: every LOD root inspected, switched and tuned in one window

**Status. Landed.** Lands after [plan_lod_blending.md](plan_lod_blending.md), whose heuristics (both kinds, distance first) and fades it shows.
The tool is built up in use: this plan is its base, and what using it shows to be missing or not working well is added as it comes up
("Added in use", at the end), not planned ahead here.

## Current state

- **The LOD window** (`src/ui/lodwindow.*`, fed in `src/apps/view.cpp`) lists rows for *streaming* roots only (levels that are payloads):
  level picker, drawn, loaded, screen size, why; the heuristic toggle and the unload delay. Render-LOD roots, the per-asset LOD the GPU
  picks per placement ([plan_render_lod.md](plan_render_lod.md)), get only a "Render LOD" section of totals and a level for the groups under the
  selection. A scene that is one render-LOD asset, such as `assets/streamtest-world/world/assets/lodtest/oak_lod8.usda`, shows no row.
- **Records and methods**: `lod` and `streaming` (streaming roots), `renderlod` and `view.renderlod {path}` (groups, live from the GPU
  readback), `view.lod.set`, `view.lod.reset`, `view.lod.heuristic` (on/off, unload delay); verbs `lod`, `lod-reset`, `lod-heuristic`,
  `lod-unload-delay`.
- **Heuristics**: today the World keeps one per root, the first readable screen-size heuristic. After [plan_lod_blending.md](plan_lod_blending.md)
  it keeps every heuristic a root authors (`LodRoot::heuristics`), and the view uses one per root by a view-wide preference, distance first
  by default (`view.lod.heuristic {prefer}`, `lod-prefer`).
- **Fixed values**: the hysteresis is a constant in the view's `levelFor` and in `shaders/lodselect.comp` (20% of the screen size, 12% of a
  distance), used where no band is authored; thresholds and bands are what is authored.

## Scope

In:
- One list of every LOD root placed: streaming roots and render-LOD groups, virtualised, filtered.
- An inspector for one root: its heuristics (every one authored: distance and screen size, which one is used under the preference and why the
  others aren't), levels (triangles, draws, residency), and its live state (the distance and the screen size, wanted, drawn, fade).
- Switching: force any root's level, render-LOD groups included, and back to the heuristic.
- Tuning: the preference (distance first, screen size first, authored order); a root's thresholds and blend thresholds, per heuristic, so
  each kind is tuned on its own and switching the preference keeps both; a global LOD bias, the global hysteresis, blending on or off;
  view-local opinions, never saved.
- Every control as an RPC method and a script verb; the inspector's data as a method.
- The selected root's heuristic drawn in the viewport: for distance, its center or bounding box and the threshold distances as rings around it;
  for screen size, the extent's sphere.

Out:
- Saving tuned values to USD: that is the editor's (`src/editor/`); the inspector shows what to author.
- Reading both heuristics and the preference itself: [plan_lod_blending.md](plan_lod_blending.md); this plan shows and drives them.
- Streaming budgets and priorities (their own plan, next on the roadmap).

## Decisions

### 1. Shape of the window

- **A. One window: the list on top, the inspector for the selected root below** (or beside, as the window is wide). The global controls in a
  header. The root follows the view's selection (the root at or above it, as the highlight does now) and a click in the list.
- **B. The list in the LOD window, the inspector a window of its own.**

**Locked A** (2026-10-25): one place for LOD; the list and the inspector change together.

### 2. What a threshold edit applies to

- **A. One placement** (the root at this path).
- **B. Every placement of the authored root** (every oak): thresholds are authored per asset, so tuning them means tuning the asset; keyed by
  the root's content and entity, which each placement knows.
- **C. Both**, B by default and A as a checkbox.

**Locked B** (2026-10-25): it is what tuning content means, and it is one key: the content the root is authored in (the stage, or the
prototype every instance shares) and the root's path in it. A single-placement override is a debugging case the forced level already covers.

### 3. The LOD bias

- **A. A factor on the measure**: 2 makes every root go finer as if it were twice as close (the distance halved, the screen size taken as
  for half the distance). Continuous, keeps the bands' proportions, the same meaning for both kinds; one number on the CPU and in the GPU
  pick's push constants.
- **B. A level offset** (+1 coarser everywhere).

**Locked A** (2026-10-25). Applied to the thresholds rather than the measure (a distance threshold times the bias, a screen size's divided by
its square), the same pick, so the GPU needs no new constant for it and the measures shown stay what the camera sees.

### 4. The GPU's per-group state while the window is open

The list shows wanted and drawn per group; for render-LOD groups those are on the GPU.
- **A. Every group's state read back each frame while the window is open** (`RenderSnapshot::readLodStates`, which exists): 16 bytes a group
  after the blending plan, about 740 KB a frame on the test world.
- **B. Only the selected group's**, and the list shows the CPU heuristic's answer for the rest.

**Locked A** (2026-10-25), measured in step 5; throttled to every few frames if it shows in the frame time.

### 5. Visualization layers

Asked for (2026-10-25): see which LOD levels are active for a mesh, and other useful layers. Locked: V1 to V5.

- **V1. The LOD Level view, with state** (`view lodlevel`): as before a colour per drawn level; on a render-LOD root's meshes, diagonal
  stripes for a forced level and a checker while it draws another level than it wants (the wanted one not resident). The flags ride in the
  G-buffer normal target's alpha beside the level tag (level + 1, plus 16 forced, 32 falling back, 64 flashing), exact in 16-bit float.
  A legend in the LOD window.
- **V2. Labels**: over each LOD root on screen, its name, drawn level (→ fading-to level and share) and measure; a click selects. ImGui
  text at the root's projected center; the list's filter applies.
- **V3. The heuristic gizmo**: for the selected root (or every one the filter shows), the distance's center or box with a ring per
  threshold at that distance (the box's rings rounded rectangles in its plane), each band's far edge a fainter ring; the screen size's sphere;
  coloured as V1's levels. `DebugDraw`.
- **V4. Pop flash**: a render-LOD group whose drawn level changes without a fade (no band, a forced level, a fallback) flashes for half a
  second, over any view, the lit one too. `lodselect.comp` sets a countdown in the group's state; the G-buffer tag carries it; the lighting
  pass tints. The `renderlod` record counts pops.
- **V5. Triangle Size shortcut**: a button in the LOD window turning the Triangle Size debug view on.

Streaming roots (levels that are payloads) show V1's level, V2 and V3; V1's flags and V4 are render-LOD only: a streaming root's state is
the CPU's, its draws would be touched on each change. A gap, named.

## Steps

1. **Every heuristic in the World**: [plan_lod_blending.md](plan_lod_blending.md) step 1 (`LodRoot::heuristics`), with the preference (its
   step 2). Nothing more here.
2. **Tuning in the view.** `ViewScene` keeps view-local opinions until it closes: `lodOverrides` by authored root and heuristic (content,
   root entity and the heuristic's index: thresholds, blend thresholds), `lodBias`, `lodHysteresis` (a fraction of the measure, by kind:
   20% of a screen size, 12% of a distance now), `lodBlending`. The heuristic (`evaluateLevels`, `levelFor`) and the
   groups' records (`setLodGroup`, refreshed as `refreshForcedLevels` does) apply them; the GPU pick takes bias, hysteresis and blending as
   push constants. Requests: `LodTuningRequest` beside `HeuristicRequest`.
   - Methods: `view.lod.override {path, heuristic?, thresholds?, blendThresholds?}` (the authored root at that path; the heuristic used when
     none is named), `view.lod.override.reset {path}`,
     `view.lod.tuning {bias?, hysteresis?, blending?}`; verbs `lod-thresholds <path> <t>…`, `lod-override-reset <path>`, `lod-bias <f>`,
     `lod-hysteresis <f>`, `lod-blending on|off`.
   - `view.lod.set` and `view.lod.reset` take a render-LOD group's path too (a forced level, `refreshForcedLevels`); today only the window's
     "under selection" control reaches groups.
3. **The inspector's data.** `ViewScene::lodRootDetail(path) -> std::optional<LodRootDetail>`: kind, levels (name, triangles and draws from
   the slots tagged with the group and level, or the level placement's for a streaming root, resident or not), default, heuristics, the
   override in force, and both measures from the posted camera (distance and screen size, whichever is used); wanted, drawn, fade level
   and amount (the GPU readback for a group, the row
   for a streaming root). On demand, when the selection changes and once a second while shown; one pass over the group's slots. Method
   `view.lod.inspect {path}` answers it.
4. **The window.** `LodWindowState` gains one row type for both kinds and the inspector. The list: kind, path, levels, wanted, drawn
   (fading), the heuristic used and its measure, why; `ImGuiListClipper` over every row; filters by text, kind, "under selection", "not drawing what it wants",
   "fading". The inspector: the detail above; a level picker (forced, or the heuristic); per heuristic, threshold and blend threshold fields,
   applied on Enter, with the authored values beside them and a reset; the header: heuristic on/off, the preference, bias, hysteresis,
   blending, unload delay. The selected root's heuristic drawn in the viewport (`debugdraw`): the distance's center or box with a ring per
   threshold, the screen size's sphere, coloured by its drawn level as the `lodlevel` view colours.
4b. **Visualization layers** (decision 5): V1 and V4 in `lodselect.comp` (forced bit and flash countdown in the state's level word, a
   pop counter in the totals), `gbuffer.vert` (the tag), `lighting.frag` (patterns, the flash tint as a push constant); V2 and V3 in
   `src/apps/view.cpp` from the published groups and the readback; `DebugDraw::circle`, `roundedRect`. Method `view.lod.visualize
   {labels?, gizmo?, gizmoAll?, flash?}`, verb `lod-show labels|gizmo|gizmo-all|flash on|off`.
5. **Measure.** With the window open on the oak: the window's frame time and the readback's cost (decision 4); at scale once the test world
   returns (Deferred).
6. **Visibility and docs.** Below; `src/view/README.md` (LOD section: the tool, the opinions), `src/world/README.md` (every heuristic kept),
   `src/renderer/README.md` (the pick's tuning constants), the Engine Map.

## Visibility

- **Methods and verbs**: those of step 2, `view.lod.inspect`; the window drives them, so an agent reaches everything the window does.
- **Records**: `lod` and `renderlod` gain per root its heuristics (every one, the one read), the override in force; a `lodtuning` field in
  `streaming` and `renderlod` (bias, hysteresis, blending, the number of overrides).
- **The LOD window** is the visibility of LOD: every root, its state live, its heuristics and levels, every control. Its viewport sphere
  shows the extent the heuristic measures.
- **Engine Map**: LOD selection and LOD select link the LOD window and name the tuning constants.
- **Trace**: `LodTuningChanged` when an override, the bias, the hysteresis or blending changes (who asked, the values): a setting changed by
  hand should be visible in a run's trace, as `LevelsWanted` is.

## Verification

- **The oak** (`oak_lod8.usda` opened alone): the list has its one render-LOD root; the inspector shows two heuristics, the distance one used
  (the default preference) and the screen-size one not used ("preference: distance first"), the other way round after `lod-prefer screen`,
  and 8 levels whose triangle counts equal the asset's `triangles_per_level`
  (15998, 6085, 2739, 1638, 473, 323, 162, 48).
- **Switching**: `view.lod.set {path: <oak root>, level: 3}` makes the `renderlod` record's group draw level 3, the window shows it forced;
  `view.lod.reset` returns it to the heuristic's level.
- **Tuning**: at a fixed camera, `view.lod.override` with the distance thresholds halved makes the wanted level the one the halved thresholds
  pick for the recorded distance; the screen-size thresholds edited the same way under `lod-prefer screen`; `lod-bias 2` the level for half
  the distance; resets restore the authored level. The same through the window's fields
  (a hand check, reported).
- **`view.lod.inspect`** answers the same measure, wanted and drawn as `renderlod` for the oak, and the same as `streaming` for the layered
  scene's streaming root.
- **Unchanged without tuning**: baselines byte-identical; the layered LOD script gives the same levels as the commit before.

## Results

Landed 2026-10-25, on the oak (`oak_lod8.usda` opened alone; distances in metres, the stage's unit), checked over RPC and in a script:

| Check | Result |
|---|---|
| Inspector | Two heuristics: distance used, screen size not ("the preference (distance first) chose heuristic 1"); the other way round after `lod-prefer screen`. 8 levels with 15998, 6085, 2739, 1638, 473, 323, 162 and 48 triangles, the asset's. At 60 m it measures 60.0 m and wants and draws level 1, as UsdLod picks. |
| Switching | `view.lod.set` level 3: the GPU draws 3 and reports it forced; `view.lod.reset` back to level 1. |
| Override | Distance thresholds and bands halved: at 60 m it wants level 3, the halved thresholds' level; the inspector shows authored 49.3 beside 24.65 in force. Descending distance thresholds are refused with a warning. The reset returns to level 1. |
| Bias | Bias 2 at 150 m picks as at 75 m: level 1 fading into 2 at 71.2%, the share UsdLod gives at 75 m; the measure stays 150 m. |
| Blending off, hysteresis 0 | At 75 m level 2, no fade, no bands in the record. |
| Pops | A walk from 40 m to 130 m in 1 m steps: 0 pops with the bands; 3 pops (one per boundary) and the flash seen with blending off. |
| Verbs and trace | Every verb in a script; one `LodTuningChanged` per change; the `lodtool` record has the tuning, layers and inspection. |
| Unchanged without tuning | three_cubes and Sponza baselines byte-identical (8 images), culling numbers equal, no warnings or errors. |
| Cost | With the window open on the oak: 0.04 ms a frame on the main thread for the window, 0.02 ms more for ImGui; the states readback is not measurable on the GPU for one group. At scale: deferred with the test world. |

Changes from the plan, and a fix:
- **Decision 2's key** is the content the root is authored in and its path there: every instance of an *instanceable* asset shares it; a
  root reached through a plain (non-instanceable) reference is the stage's and its own.
- **Bias on the thresholds**, not the measure (decision 3): the same pick, no new GPU constant, and measures show what the camera sees.
- **The old "groups under the selection" control** is gone: the list's "under selection" filter with "Set shown" does it, for both kinds.
- **The pop flash** lasts half a second at the view's frame rate (a push constant, at most 127 frames), not a fixed frame count: offscreen
  runs far above 60 fps.
- **Fix to the blending change**: `gbuffer.vert`'s fade coverage at location 6 was unread by the opaque `gbuffer.frag`, a validation
  warning per opaque pipeline in every run since. Tag and coverage now share one `uvec2` varying.
- **Not checked headless**: the labels (ImGui is not in screenshots) and clicking one, the window's fields: hand checks.

## Open questions

- Should overrides survive a scene reload in the same view (the gizmo's opinions don't)?
- The inspector could show what the levels cost (triangles per pixel of screen size) to help pick thresholds. Worth it now, or after a first
  round of tuning by hand?

## Added in use

What using the tool shows to be missing or not working well, added as it comes up, each with the date and what prompted it. Empty until
the base lands.

## Deferred / follow-ups

- **Write tuned thresholds back to USD.** Trigger: `ngen-editor`, which owns USD edits.
- **Per-placement overrides** (decision 2 A). Trigger: a debugging case the forced level doesn't cover.
- **The tool at scale** (thousands of groups: the list's clipping, the readback each frame, decision 4). Trigger: the test world's re-authored
  LODs land.
- **A heuristic graph** (screen size over distance with the thresholds and bands drawn). Trigger: tuning by numbers proves hard.
