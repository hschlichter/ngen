# Streaming domain: residency from the authored load and unload radii

**Status. Draft.** Decisions 1, 2 and 5 locked 2026-10-30 (Henrik); 3, 4 and 6 as leaned, pushback welcome. First of three plans: this one
(what is resident), [plan_streaming_tool.md](plan_streaming_tool.md) (the window and overlays), [plan_streaming_budgets.md](plan_streaming_budgets.md)
(what fits).

## Current state

The streaming test world was regenerated (2026-10-29) to the format its spec describes, `assets/streamtest-world/docs/LOD_STREAMING.html`:
a two-level grid of LOD roots (100 regions of 1000 m with `full` · `medium` · `proxy`, 1600 tiles of 250 m with `full` · `medium` ·
`simplified`), assets with 8 render-LOD levels inside tile `full`. Each cell authors its heuristics in a sibling `LODHeuristics/<cell>`
prim, by domain: `imaging` (a distance and a screen-size heuristic, with blend bands), `physics` (tiles), and a custom `streaming` domain: a
`LODDistanceHeuristic` whose `thresholds` are the load radii and whose custom `streaming:unloadThresholds` are the unload radii. The spec's
residency rule (§7): per root a state *s*, the finest level that must be resident, from the load radii going finer and the unload radii
going coarser; level *s* and every coarser level stay resident; a root's children are visited only while *s* is 0. The spec's authoring
contract (§4) is what makes that enough: imaging threshold ≤ load radius ≤ unload radius per boundary, a child's thresholds inside its
parent's first boundary.

Run on the new world at `/cameras/village` (2026-10-30), the engine already handles its structure: the heuristics in the sibling prims, the
region `full` level as a reference, distance to the bounding box, drawing a level whatever its authored visibility, the assets' render LOD
on the GPU, ground cover, per-cell material inputs. 192 of the 196 cells draw the level the manifest expects; the 4 others are inside a blend
band, where a streaming root draws the finer level ([plan_lod_blending.md](plan_lod_blending.md)).

What it doesn't do is residency as the world authors it ([plan_streaming_v2.md](plan_streaming_v2.md) and
[plan_streaming_districts.md](plan_streaming_districts.md) built what there is):
- **The streaming heuristic isn't read.** The packer drops `streaming:unloadThresholds` (`isEngineAttribute`, `src/asset/pack/usd/usdpacker.cpp`),
  and the composer marks every heuristic outside `imaging` unusable (`worldcomposer.cpp`, "its domain is …").
- **What loads is the imaging pick, the coarsest level, and levels kept for an unload delay** (`ViewScene::evaluateLevels`, `keepLevel`,
  `releaseLevels`; `WorldComposer::loadedLevels`): 10 s on a clock, then released. Nothing loads before imaging wants it, and an authored
  in-between level is resident only by chance. Moving from inside `R_4_5` to 691 m away, the region wanted `medium`, which wasn't loaded,
  and drew `proxy` for 235 frames while `full` was still loaded: `full` → `proxy` → `medium`.
- **Nothing checks the authoring contract or measures streaming**: late draws, lead time, reloads.

[plan_streaming_lod.md](plan_streaming_lod.md) decision 4 has eviction hysteresis as "a payload loaded stays at least N seconds"; decision 1
here replaces that part.

## Scope

**In**
- The packer keeps `streaming:` attributes; the World reads the `streaming` domain beside `imaging` (load and unload thresholds).
- Residency per root from its streaming heuristic, as the spec's §7. A root without one: the imaging pick and every coarser level.
- No time in residency: the unload delay and everything serving it removed.
- Drawing as the spec's §6: the picked level, else the nearest coarser resident one.
- The authoring contract checked per root and reported, not worked around.
- Measurements in the `streaming` record, and a `fly` verb to fly a camera path at a speed.
- Records, RPC methods and verbs for all of it.

**Out**
- The streaming window, overlays and configuration UI: [plan_streaming_tool.md](plan_streaming_tool.md). This plan keeps the LOD window and
  Scene Flow working (the delay's fields removed) and nothing more.
- Budgets and request priority: [plan_streaming_budgets.md](plan_streaming_budgets.md).
- Caldera: not a target of this plan.
- Cancelling requests in the asset client (decision 3). Physics: the engine has none; the `physics` heuristics stay listed, not read.
- Cross-fading cell levels (decision 4).

## Decisions

### 1. No time in residency — locked

What is resident is a function of the camera's position, what is authored (the radii) and each root's previous state (the hysteresis).
Nothing loads or unloads because time passed. The unload delay was the districts world's stand-in for two things this world authors or a
later plan owns: hysteresis on unloading (the unload radii) and keeping what the camera may return to (budgets). Load latency is answered by
authoring (the lead between a load radius and the imaging threshold) and by budgets and priority, not by a clock. Time appears only as a
measurement. Locked 2026-10-30.

### 2. Every authored level is part of the chain — locked

The state's level and every coarser level are resident: a tile drawing `full` keeps `medium` and `simplified`, a region drawing `full` keeps
`medium` and `proxy`. So every authored level is a ready fallback in both directions: coming closer a cell steps through its levels in order,
moving away the coarser level is there before the finer one stops being wanted. Nothing treats a level as special by its position: no
"coarsest is never let go" rule, no meaning given to `full` or `proxy`; levels are indices, shown by their authored names. The cost is memory:
the spec measures about 1.5× the resident set (§11). Locked 2026-10-30.

### 3. Unwanted requests: dropped on arrival, not cancelled

- **A. Drop on arrival**: a pack that arrives for a level no longer resident isn't placed; it is counted (`arrivedUnwanted`). Nothing new in
  the asset client.
- **B. Cancel**: the asset client and server learn to cancel an in-flight request.

I lean **A**: it is what the scene thread already does with a pack nothing names, and the count says whether B is worth it. B is Deferred.

### 4. Cell blend bands: the finer level, no cross-fade

- **A. As now**: inside a band a streaming root draws the finer level, which the spec allows (§5: "or cross-fade").
- **B. Dither between the two cell levels**, as render LOD does.

I lean **A**: B draws two cell levels in the band (a tile's `full` and `medium` together) and needs both resident and ready; it waits until
pops at cell switches show they matter.

### 5. The system stays honest: no patches for authoring gaps — locked

The engine does what the authoring says and shows where that falls short; it doesn't paper over it. Locked 2026-10-30.
- **Drawing** is the spec's §6: the picked level when it is ready, else the nearest ready coarser level, else nothing (a hole, counted). A
  finer level that happens to be loaded isn't drawn in its place.
- **Residency** is the state's level and every coarser one; a picked level finer than the state (an imaging threshold past its load radius)
  isn't loaded early to cover it. It draws late, and the record says so.
- **The authoring contract is checked**, per root, and a violation is a finding in the records with the numbers (step 2), not a correction.
- The one exception is tooling: a level set by hand (`lod`, the LOD window) is resident with every coarser one, as the tool asks for it.

### 6. What the streaming heuristic may be

- **A. Either kind**: a distance heuristic (load and unload radii, unload ≥ load) or a screen size (unload ≤ load), read as `pickLodLevel`
  reads imaging.
- **B. Distance only**, as the world authors it.

I lean **A**: the state rule is the same count over thresholds for both, and refusing one kind would be a rule the spec doesn't make.

## Steps

1. **Packer**: `isEngineAttribute` keeps names starting with `streaming:`. The world repacks with no new warnings.
2. **World** (`src/world/world.h`, `worldcomposer.cpp`):
   ```cpp
   enum class LodDomain : uint8_t {
       Imaging,
       Streaming,
       Other, // physics, audio, anything else: kept to show
   };

   struct LodHeuristic {
       // … as now, with:
       LodDomain domainKind = LodDomain::Other;
       std::vector<float> unloadThresholds; // streaming:unloadThresholds; empty: the load thresholds (no hysteresis)
   };

   auto chooseLodHeuristic(const std::vector<LodHeuristic>& heuristics, LodDomain domain, LodPreference preference) -> int32_t;

   // The spec's residency state: the finest level that must be resident, from the load thresholds going finer and the unload thresholds
   // going coarser than `previous` (none: the load thresholds alone).
   auto streamingState(LodHeuristicKind kind, const std::vector<float>& load, const std::vector<float>& unload, float measure, uint32_t levelCount, std::optional<uint32_t> previous) -> uint32_t;

   // The spec's authoring contract for one root: what doesn't hold, each with the numbers.
   struct LodAuthoringFinding {
       std::string rule;    // "imaging threshold > load radius", "load radius > unload radius", "child threshold outside parent", …
       uint32_t boundary = 0;
       std::string detail;  // the values compared
   };
   auto checkLodAuthoring(const LodRoot& root, const LodRoot* parent) -> std::vector<LodAuthoringFinding>;
   ```
   `unusable` per domain: a streaming heuristic whose unload thresholds don't match the load ones in count, or lie on the wrong side of
   them; `physics` and `audio` say "not read: the engine has no physics" / "no audio". `ngen-worldcheck` and `ngen-usdcheck`
   `--lod-heuristics` print the unload thresholds; `ngen-worldcheck --lod-authoring` prints the findings over the whole world.
3. **Composer**: `setLodLevel`, `resetLodLevel` and `keepLodLevels` become one opinion per root:
   ```cpp
   // The levels a streaming root keeps loaded, set by the reader; without one, its default level and every coarser level.
   auto setResidentLevels(const std::string& rootPath, std::set<uint32_t> levels) -> void;
   auto resetResidentLevels(const std::string& rootPath) -> void;
   ```
   `loadedLevels` returns that set (a prototype's levels as now). The composer no longer adds the coarsest level.
4. **Residency in the view** (`ViewScene::evaluateLevels`): per placed root, `selected` (the imaging pick, as now) and `state` (the streaming
   heuristic's `streamingState`, its previous state kept per root while the root is placed). Resident: `state` and every coarser level, or
   from a hand-set level when that is finer. A root without a streaming heuristic: `selected` and every coarser level. A root leaves
   `placedRoots` and its state goes, so a region not at `full` holds no tile state. Removed: `keepLevel`, `releaseLevels`,
   `updateLevelsDue`, `levelsDue`, `levelsKept`, `unloadDelaySeconds` and the scene thread's timed wake, `HeuristicRequest::unloadDelay`,
   `setUnloadDelay`, the `lod-unload-delay` verb and `{unloadDelay}` on `view.lod.heuristic`, and their fields in the LOD window and Scene
   Flow. `LevelsWanted` becomes `ResidencyChanged` (levels requested and released, roots finer and coarser); `LevelsUnloaded` says the levels
   went outside their unload radius.
5. **Drawing** (`drawnLevelOf`, `src/view/placement.cpp`): decision 5. A root drawing coarser than `selected` is late; one drawing nothing is
   a hole.
6. **Measurements**, in the `streaming` record:
   - per root: `selected`, `state`, the streaming heuristic used (or why none), `resident`, `drawn`, `late`, its authoring findings, and per
     level `requested`, `resident`, `drawn`, and its lead (seconds from resident to drawn, the last time it switched in);
   - totals: roots late and holes now, late switches, root-seconds drawn coarser, loads and reloads (a level loaded again after it was
     released this session), lead min and median, `arrivedUnwanted` (decision 3), authoring findings.
   `view.streaming.reset` zeroes the counters, for a run measured from a known start.
7. **`fly`**: `fly <camera path prefix> <speed m/s>` (`view.camera.fly`) moves the camera through the cameras whose paths start with the
   prefix, in path order (`/cameras/flight_00` …), at the speed, eye and direction interpolated; it answers when the last camera is reached.
   `fly-stop` stops it. The camera's motion is the only thing in this plan that runs on time, and it is a test driver: it moves the camera,
   it decides nothing.
8. **READMEs**: `src/world/README.md` (domains, `streamingState`, the authoring check, resident levels set by the reader), `src/view/README.md`
   (residency, drawing, measurements, `fly`; the unload delay removed), the `run-headless` skill (`fly`, the new record fields).

## Visibility

- **Records**: `streaming` (per root and totals, step 6), `lod` and `lodtool` (the streaming heuristic, its thresholds as authored and in
  force), `scenethread` (released levels, no kept levels).
- **RPC and verbs**: `view.camera.fly` / `fly`, `fly-stop`; `view.streaming.reset`. Removed: `lod-unload-delay`,
  `view.lod.heuristic {unloadDelay}`.
- **Tools**: `ngen-worldcheck --lod-authoring`.
- **Trace**: `ResidencyChanged`, `LevelsUnloaded` (reworded).
- **Windows**: none new here; the LOD window and Scene Flow lose the delay's fields. The streaming window and overlays are
  [plan_streaming_tool.md](plan_streaming_tool.md)'s, built on this plan's records.

## Verification

Pass or fail is decided by checks at fixed camera positions, which give the same result on every run and machine. The flight is a
measurement, weighted below.

- **Packing**: `ngen-worldcheck --lod-heuristics` lists each tile's `streaming_distance` usable with load [300, 500] and unload [360, 580],
  each region's with [700, 1400] / [800, 1600]; `physics_distance` listed as not read. `ngen-usdcheck` prints the same.
- **Authoring check**: `ngen-worldcheck --lod-authoring` on the world reports no findings (its own `validate` passes the same rules); with
  a tile's load radius tuned under its imaging threshold (`lod-thresholds`), the `streaming` record shows that finding for the tile.
- **Selection unchanged**: at `/cameras/village`, the cells drawn match the manifest's `expected_cells` but the same 4 in-band cells.
- **The chain**: at `/cameras/village`, every placed root's `resident` is `state` and every coarser level; tile `R_4_5/T_3_0` (289 m) has
  state 0 (inside the 300 m load radius) and draws `medium` with `full` resident (prefetched).
- **No pop past a level**: the move from `/cameras/village` to `-1700,500,50`: `R_4_5`'s `drawn` goes 0 → 1 with no record frame drawing 2.
- **Hysteresis by distance**: a tile approached to 250 m, then the camera at 330 m: `full` stays resident; at 370 m: released, with
  `LevelsUnloaded`.
- **No time**: `rg 'unloadDelay|levelsKept|keepLodLevels|levelsDue' src` finds nothing; the same script run twice gives the same `state`
  and `resident` per root at the same script frames.
- No warnings or errors in the trace of any run.

**The flight, as a measurement.** `fly /cameras/flight_ 30` over the whole path after `view.streaming.reset`, release build:
- *Pass or fail*: the fly completes, no hole after the first `SceneResident`, no warnings or errors.
- *Reported, not judged*: late switches, root-seconds coarser, loads and reloads, lead, `arrivedUnwanted`, written in Results with how they
  were taken (build, warm or cold asset cache, machine) beside `world/stream_replay.json`'s. The two aren't the same experiment: the replay
  simulates one channel at a fixed bandwidth with the files' sizes, the engine loads packs from a local server whose speed depends on the
  cache and the machine; frame rate moves the camera's sampling too. A difference is a question to look into, not a failure. The flight is
  for comparing the engine with itself across changes, under the same conditions.

## Deferred / follow-ups

- **Cancelling requests** in the asset client and server (decision 3 B). Trigger: `arrivedUnwanted` shows bandwidth worth saving on a
  flight.
- **Layer packs released with their level**: a level's scene description stays held after the level is released. Trigger: the memory
  budget in [plan_streaming_budgets.md](plan_streaming_budgets.md).
- **Cross-fading cell levels** (decision 4 B). Trigger: visible pops at cell switches.
- **Physics domain**: collision levels by the `physics` heuristics. Trigger: physics in the engine.
- **The camera's focal length**: the view's 45° ignores the cameras' aperture and focal length, which the screen-size heuristics are
  calibrated for. Trigger: preferring screen size for this world.
