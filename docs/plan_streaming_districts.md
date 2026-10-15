# Streaming districts

**Status. In progress.** Decisions 1 to 8 locked 2026-10-15, each as recommended; steps 1 to 5 done 2026-10-15, the heuristic capped (default 2 roots finer) until budgets come.

Step 3 of [plan_streaming_lod.md](plan_streaming_lod.md) ("districts stream") and step 1 of the roadmap's vertical slice ("Streaming,
minimal"): every LOD root's wanted level comes from the camera each frame through its UsdLod heuristic, levels load and unload by it, and
the coarsest level of every root stays resident as the floor. Step 2 of that plan (Caldera's proxy map through the scene pack) is in place
except one visibility gap, closed here (step 1 below).

## Current state

- **Levels are chosen by hand only.** A root wants `lod:default:index` until `view.lod.set` or the LOD window sets one
  (`World::lodLevels`); nothing evaluates a heuristic. The World keeps a root's `thresholds` and its heuristic's entity, not the
  heuristic's `extent` or `projectionMethod`, which the packer keeps (`isEngineAttribute`) and every Caldera heuristic authors.
- **The floor exists**: a root loads its wanted level and its coarsest (`WorldComposer`, `loadedLevels`), and draws the finest loaded level
  not finer than the wanted one (`World::drawnLevel`).
- **Every change recomposes the whole World.** `composeOnce` starts from an empty World each round (`world = World()`, the prim index memo
  and prototypes cleared), and every compose is followed by a full extract. Measured on Caldera (`scenethread` record, 2026-10-15):

  | | compose | extract | draws |
  |---|---|---|---|
  | default (every root at its coarsest) | 186 ms | 624 ms | 4,427 |
  | `map_tile_p` at level 0 | ~5 s per round, 11 rounds | ~3 s | 266 k |
  | after `lod-reset` | | 1.1 s | 4,427 |

  The district took 48 s from the request to composed, almost all of it re-composing 934 k prims and 1,404 prototypes eleven times (one
  round per level of arcs). Packing and delivery come from the cache and are not what's slow.
- **Nothing is released.** After the reset the scene thread still holds 47,870 mesh packs and 5,096 layer packs; only the GPU side drops
  what isn't drawn.
- **Payload placements** (`PayloadPlacement`: asset ids, loaded, `extentsHint`) are in the World and in no record or window.

## Scope

In:
- The screen-size heuristic (UsdLod's `LODScreenSizeHeuristic`, `projectedSphere` and `projectedExtent`) evaluated per root from the view
  camera, on the scene thread, giving each root's wanted level.
- Composition that doesn't redo what didn't change: composed content cached across composes and reused (decision 2), and extract per
  cached content (decision 3).
- Unloading: a level no longer wanted (and not the floor) leaves the World, the libraries, the pack maps and the GPU.
- The switch rule, hysteresis, the unload delay and how a hand-set level interacts with the heuristic (decisions 4 to 7).
- Visibility: a `streaming` record, payload placements, the LOD window and view showing heuristic choices, Engine Map nodes, trace events per
  batch.

Out:
- Budgets, priorities, eviction under memory pressure ([plan_streaming_lod.md](plan_streaming_lod.md) decisions 4 and 7): the next streaming
  step.
- Texture streaming (that plan's step 5) and model-level streaming beyond what nesting gives for free (its step 4: model roots inside a loaded
  district are roots like any other and follow the same heuristic here; tuning them is step 4's).
- The distance heuristic (`LODDistanceHeuristic`): Caldera authors only screen size. A root whose heuristic isn't screen size keeps its
  default level and is reported.
- Blending between levels (`blendThresholds`), level fades.
- Meshlets and occlusion (vertical slice step 2).

## Decisions

Locked 2026-10-15, each as recommended: 1 A, 2 A, 3 A, 4 B, 5 A, 6 A, 7 A, 8 B.

### 1. Where the heuristic runs

- **A. On the scene thread**, from the camera the main thread posts each frame (latest only, not queued: an older camera is never applied).
  The scene thread owns the World and the roots' world transforms; it evaluates every reached root after each batch and when the camera moved,
  and the wanted levels it computes go into the composer as today's hand-set ones do.
- **B. On the main thread**, from the snapshot's roots. The main thread has the camera, but not the heuristics' extents in world space for
  roots inside prototypes, and every change would go back to the scene thread as a request anyway.

**Locked: A.** The data is there and the result is used there. The camera is a frustum (view-projection, position) in metres, the same one the
view draws with.

### 2. Composition that doesn't redo everything

- **A. Cache composed content across composes.** Prototypes are already composed once per key (their arcs); keep them, and LOD level
  contents keyed the same way, across composes instead of clearing them. A compose then composes only content whose packs it hasn't composed
  before, and a root switching level swaps which cached content its level slot points at. A content records the layer packs it read, and a
  refreshed pack (repack, change notice) drops the contents that read it.
- **B. General incremental composition**: memoize prim indexes across composes and recompute what changed packs touch, as USD's Pcp change
  processing does. The most general, and the most work and risk.
- **C. Keep full recomposition**, batch level changes and accept the latency. Measured: 48 s for one district; not usable for flying.

**Locked: A.** It follows the units the data already has (an instance's prototype, a root's level), it is what HLOD means (a level is a unit that
comes and goes), and it builds on the instancing machinery `WorldComposer` already has. It needs to be checked against composition rules: a
level's content is cached by the nodes of its prim index, as a prototype is, so an opinion from the stage's own layers under a level's path
is part of its key, and a content shared by two roots is only shared when their indexes are the same. `ngen-worldcheck` against
`ngen-usdcheck` stays byte-identical (with `--lod` levels set) as the guard.

### 3. Extract per content

- **A. Draws cached per content**: a content's draws (mesh, material, submesh, transform relative to the content) are built once and placed
  with each use's transform; a level switch removes one content's draws and adds another's. The full walk stays for what isn't in a cached
  content (the stage's own prims), which on Caldera is small.
- **B. Keep the full walk**, made faster. 266 k draws in 3 s would need to become milliseconds; a walk can't get there.

**Locked: A**, with A of decision 2. Slots keep their keys (prim path and submesh), so the renderer sees the same change lists it sees today.

### 4. The switch rule

[plan_streaming_lod.md](plan_streaming_lod.md) decision 6 (locked) draws a finer level as soon as its own payload is resident, and the
content under it arrives payload by payload. For a district that is the pop seen in the LOD window: the proxy disappears and thousands of
models fill in over seconds.

- **A. As decision 6**: switch when the level's payload is in.
- **B. Switch when the level is complete**: a root keeps drawing its coarser level until everything its finer level wants is resident
  (payloads, packs, GPU uploads), then switches in one frame. One rule for every root: for a model root whose level is one payload, A and B
  are the same. UsdLod allows it ("renderers may blend between multiple children or even display all of them").

**Locked: B.** It revises [plan_streaming_lod.md](plan_streaming_lod.md) decision 6, which is marked there. Its cost: a level that
never completes (a failed asset) never shows; it would switch on completion or on its last request answering, failed or not.

### 5. Hysteresis

A camera hovering at a threshold would flip a root between levels every frame, each flip a load or unload.

- **A. A band around each threshold**: switching finer at the threshold, coarser only below it by a margin (say 20% of the threshold).
- **B. A minimum time** between a root's switches.
- **C. Both.**

**Locked: A.** B adds latency to every switch, and A alone removes the flipping.

### 6. Unloading

- **A. A level unwanted for a while is unloaded**: its content leaves the World (and its cached content, decision 2), its draws and library
  entries go, and its packs are released. A delay (say 10 s) keeps a level the camera turns away from and back to; the floor is never unloaded.
- **B. Never unload** in this step; memory grows until budgets come.

**Locked: A**, with the delay: without it a flight over the island holds every district it passed, which is what "streaming" is meant to prevent.
Budgets (the next step) decide what goes first when memory is short; this step only unloads what nothing wants.

### 7. Hand-set levels

- **A. A hand-set level overrides the heuristic** for that root until `lod-reset`. The LOD window shows which roots are hand-set. A global
  switch turns the heuristic off (every root at its default or hand-set level), for looking at levels without the camera changing them.
- **B. The heuristic always wins**; hand-set levels only while it is off.

**Locked: A.** It keeps the LOD window useful for inspecting one root while everything else streams.

### 8. Off-screen roots

UsdLod's screen size is 0 for a root wholly outside the frustum, so it wants its coarsest level: turning around unloads what was behind.

- **A. As USD**: off-screen is coarsest; decision 6's delay keeps what is turned away from briefly.
- **B. Ignore the frustum** (screen size from distance only), so what is near stays loaded whichever way the camera looks.

**Locked: B** for this step: a flight turns constantly, and near districts reloading at every turn is the wrong behaviour for an open world. The
budgets step can lower priority behind the camera ([plan_streaming_lod.md](plan_streaming_lod.md) decision 4 already says so). This departs
from USD's reference query, which the `streaming` record states per root (`screenSize`, `inFrustum`).

## Steps

1. **Payload placements visible.** A `placements` section in the `lod` record (or a `payloads` record): every payload placement's path,
   asset ids, loaded, `extentsHint` in world metres; in the LOD window, a root's level payloads and their state. Closes step 2's gap.
2. **Heuristic data in the World.** `LodRoot` gains `extent` (the heuristic's, local to the root), `projectionMethod`, and its heuristic's
   type (`src/world/world.h`, `worldcomposer.cpp` where `thresholds` are read). `ngen-worldcheck`'s fingerprint is unchanged (it doesn't
   print LOD data).
3. **Cached contents across composes** (decision 2) in `WorldComposer`: prototypes and level contents keyed by their nodes, kept across
   `composeOnce`, each with the layer packs it read; `addPack` of a pack already held (a refresh) drops the contents that read it. Checked
   against `ngen-usdcheck` on `assets/layered/` and Caldera with `--lod` levels.
4. **Extract per content** (decision 3) in `ViewScene::extract`: a content's draws built once, placed per use; a level switch adds and
   removes one content's draws.
4b. **Payloads as contents** (added 2026-10-15, after step 3's results): every payload, not only a LOD level, composes as a content of its
   own, kept and linked as levels are, so a compose round recomposes only the contents that are still missing packs instead of a whole
   district's layout ([plan_streaming_lod.md](plan_streaming_lod.md) decision 3: payloads are what streams). Verified by the first switch
   of `map_tile_p` to level 0: each round's `levelsComposed` holds only what is new, and the time to composed falls well below 48 s.
5. **The heuristic** (decision 1): `ViewScene::setCamera(frustum, position)`, latest only; after each batch and when the camera moved, the
   scene thread computes each reached root's screen size and wanted level (decisions 5, 7, 8) and sets the composer's levels. The view posts
   the camera each frame.
6. **The switch rule and unloading** (decisions 4 and 6): a root's drawn level advances only when its finer level is complete; levels
   unwanted for the delay are unloaded from the World, the content cache, the libraries and the pack maps.
7. **Visibility** (below) and the docs: `src/world/README.md` (cached contents), `src/view/README.md` (the heuristic, unloading, the
   `streaming` record), the Engine Map.

### Steps 1 and 2 results (2026-10-15)

- The `payloads` record lists every placement reached while drawing; on Caldera's default open, 836, all loaded, 182 without `extentsHint`
  (the LOD levels, whose bounds are their root's heuristic extent). A placement under a level that isn't loaded isn't reached; the `lod`
  record's `levelPayloads` names each level's own payloads, and the LOD window's row tooltip shows them with their state.
- `LodRoot` reads the heuristic's type, `extent` (UsdLod's fallback when unauthored) and `projectionMethod`; all 182 of Caldera's roots
  are `LODScreenSizeHeuristic`, and `map_airfield`'s extent and thresholds in the `lod` record match `usdcat`'s.
- `ngen-worldcheck` against `ngen-usdcheck` on `assets/layered/root.usda`, default and `--lod /World/Tree/lod=0`: the one line that
  differed before (`/World/Inherited`, an inherit, which the World doesn't compose) and nothing else. Baselines byte-identical.

### Step 3 results (2026-10-15)

- **Levels as contents, linked at resolution**: a LOD root's levels are stand-ins linked to level contents (the level prim, loaded or not,
  and everything under it); prototypes and level contents are kept across composes by key, a content with a gap composed again in place;
  layer stacks kept and rebuilt in place. A changed pack drops everything kept.
- **Unchanged composition**: `ngen-worldcheck` against `ngen-usdcheck`, transforms and instancing: `assets/layered/root.usda` at the default
  and at both levels of `/World/Tree/lod` (only the known `/World/Inherited` line); Caldera's default open (54,124 prims) and with
  `map_tile_p` at level 0 (934,429 prims, 1,404 prototypes): identical. A repack on the layered scene before and after a level change gives
  the same World; the eight baselines are byte-identical; no warnings.
- **Measured on Caldera** (`scenethread` record's `compose.last`):

  | | compose | of it: stage, resolution | contents composed | extract |
  |---|---|---|---|---|
  | default open | 143 ms | 113 ms, 2 ms | | 164 ms |
  | `map_tile_p` to level 0, first time | ~4.9 s a round, 11 rounds | 304 ms, 4.6 s | 4 levels, 2 prototypes in the last | 4.0 s |
  | back to the default | 338 ms | 284 ms, 5 ms | none | 1.3 s |
  | to level 0 again | 271 ms | 190 ms, 38 ms | none (9,662 reached, all kept) | 1.8 s |

  Switching back to kept content composes nothing; the extract (step 4) is what's left. The first switch is still slow: a level content
  with a gap is composed again whole each round, and a district's level is its whole layout (its non-instanced prims), so each of the 11
  rounds recomposes it.

### Step 4 results (2026-10-15)

- **Kept walks per content** (decision 3 A): each content's walk is kept by version and inherited state (purpose, binding) and replayed per
  use, in the order a full walk gives; level links are read at use (resolution changes them without changing the content). Meshes and
  materials are resolved once per rebuild per mesh and binding. Prim transforms come from a `TransformIndex` (per content, plus every use),
  not a map of every prim; what a rebuild builds is one shared `SceneDraws`, so a snapshot no longer copies it; the prim count is kept per
  content version instead of a walk per publish.
- **Unchanged output**: the eight baselines byte-identical; on `assets/layered/`, select inside an instance and in a drawn level, the
  undrawn level refused, `translate` of an instance and of a prim inside it move the framing as before; Caldera's draws 4,427 and 266,330
  as before; no warnings.
- **Measured** (Caldera, `map_tile_p` back to level 0 from kept content; runs vary by up to 40%): extract 0.9 to 1.6 s (from 1.8 to 3 s),
  of it slots 0.35 to 0.6 s; publish about 10 ms (from up to 0.9 s); 51,163 content walks reused, 1 walked (the stage). The replay still
  produces 266 k draws one by one (about 1 to 2 microseconds each: path, key, bounds, slot): the cost is now the number of draws, not of
  prims. The plan's "within a second" isn't reached: compose 0.3 to 0.4 s plus this extract, plus the uploads step 6 will keep (560 MiB of
  meshes the reset dropped from the GPU).
- **Decided 2026-10-15: accept it for now** and go on with 4b and 5. Keeping each use's draws across rebuilds, so a rebuild touches only the
  uses that changed, is under Deferred, triggered by the flight test.

### Step 4b results (2026-10-15)

- Every prim with payloads (in a content that loads them) is a stand-in linked to a content of its own, composed at resolution in its own
  window, so what its payloads miss is its own gap; `Entity::levelContent` and `World::levelContents` are renamed `linkedContent` and
  `linkedContents` (a LOD level or a prim with payloads).
- **Unchanged composition**: `ngen-worldcheck` against `ngen-usdcheck` identical on `assets/layered/` (the known inherit line) at both
  levels, Caldera's default, and Caldera with `map_tile_p` at level 0 (934,429 prims); baselines byte-identical; the layered checks as before.
- **Measured**, first switch of `map_tile_p` to level 0: the last compose round composed 10 contents and 2 prototypes in 0.5 s (4.9 s
  before); `ngen-worldcheck` composes the district in 9.7 s (14.3 s after step 3, 48 s before); in the view, request to resident about 40 s
  (58 s before). What remains is extract: every batch of arriving assets is followed by a full extract (12 during this load, about 2 s each,
  so about 24 s), step 4's cost per draw times how often it runs.

### Paced extracts (2026-10-15, after 4b)

- While assets arrive, an extract waits at least as long after the last as that one took (250 ms at least); the drawn scene's assets are
  requested from the kept walks, and only after a compose, a material's arrival or a purpose change (it walked the whole World before
  every extract). Baselines byte-identical; the layered checks as before.
- First switch of `map_tile_p` to level 0: 9 extracts during the load instead of 12 (586 batches held back); request to resident about
  34 s (40 s before, 58 s before 4b). Composition ends about 29 s after the request, 11 rounds of about 2.6 s, most of it waiting for
  layer packs: as far as the timeline shows, each round's layer packs queue at the asset server behind the tens of thousands of mesh
  requests sent between rounds (not measured on the server). That is request priorities ([plan_streaming_lod.md](plan_streaming_lod.md)
  decision 7), the budgets and priorities step.

### Step 5 results (2026-10-15)

- **The heuristic** (decisions 1, 5, 7, 8): `ViewScene::setCamera` (latest only), evaluated on the scene thread after the camera moved, an
  extract or a level change, over the roots the last extract reached: UsdLod's projected-sphere screen size without the frustum, a 20% band
  before going coarser, hand-set levels winning, a global toggle. Changed levels go to the composer, which links again without composing the
  stage (`WorldComposer::relink`). The view posts the camera only once it is placed, not while it follows the scene loading (it flipped
  the layered scene's tree five times while auto-framing before that).
- **Visibility**: the `streaming` record (per root screen size, wanted and why, drawn, loaded; the evaluation's counts), `lod-heuristic` and
  `view.lod.heuristic`, the LOD window's toggle and its Screen and Why columns, a "LOD selection" node in the Engine Map, `LevelsWanted`.
- **Layered scene**: the tree wants level 0 up close (screen size 3.25 against 0.25) and loads and draws it, level 1 from far, its default
  when off; a hand-set level wins. Baselines byte-identical.
- **Caldera from `map_capital_square`: out of memory.** The first evaluation set 35 of the 182 roots finer at once, every district and
  terrain section near the camera: a district's extent is kilometres across, so from inside the island its sphere is several times the
  view at the converter's threshold of 0.25, and without the frustum the ones behind count too. `ngen-view` reached 10.8 GB resident and the
  kernel killed it. One extract during that load took 33 s (not looked into yet). Needs a decision before the heuristic can stay on for
  Caldera: see Open questions.

### Step 5's cap (2026-10-15, decision for the open question above: a cap until budgets come)

- At most N roots finer than their coarsest by the heuristic (default 2; `lod-cap`, `view.lod.heuristic {cap}`, the LOD window's Cap): the
  roots the camera is inside first, smallest first, then the largest on screen; the others "capped". A root whose heuristic extent is empty
  keeps its default: Caldera's 14 terrain sections (`st_*`) author the empty box (`FLT_MAX` to `-FLT_MAX`), a converter issue in the fork.
- Caldera from `map_capital_square` (peak resident memory, a watcher stopping the view at 10 GB): cap 3 before the ranking rule picked the
  vista ring (its sphere around the camera measured 3,280) and two districts and passed 10 GB; cap 1 and 2 peak at about 4.2 GB (cap 2:
  `map_tile_o` and `mv_intel`). The capital itself stays capped at the square: the camera isn't inside its heuristic extent, and the
  others outrank it. A count is a stand-in: a district at level 0 ranges from a few thousand draws to over 200 thousand.

## Visibility

- **`streaming` record**: per root its screen size, in frustum, wanted level and why (heuristic, hand-set, heuristic off), drawn level,
  loaded levels, and for a level loading: payloads and packs wanted, in flight, resident; totals; the heuristic's state (on, off).
- **`lod` record and LOD window**: the heuristic's choice per root (screen size, wanted by the heuristic or by hand); a "Heuristic" toggle
  (decision 7); a root's level payloads (step 1). The LOD level view already colours the drawn level.
- **`scenethread` record and Scene Flow**: cached contents (count, hits, composed this round), unloads, packs released.
- **Script verbs and RPC**: `lod-heuristic on|off` (`view.lod.heuristic`), the existing `lod` and `lod-reset`.
- **Trace**: per batch, not per root: `LevelsWanted` (roots changed, finer and coarser), `LevelsSwitched`, `LevelsUnloaded` (packs
  released, bytes).
- **Engine Map**: a "LOD selection" node on the scene thread (camera in, wanted levels out to compose) and the content cache in Compose's
  description.

## Verification

- **Composition is unchanged**: `ngen-worldcheck` and `ngen-usdcheck` byte-identical on `assets/layered/root.usda` and on Caldera, with and
  without `--lod` levels.
- **Composition is fast after the first time**: on Caldera, setting `map_tile_p` to level 0, back, and to 0 again, the third compose
  composes no content (the `scenethread` record's cache hits), and a district switch from cached content is drawn within a second.
- **A scripted flight**: a camera path from `map_capital_overview` down into the capital and out over the island, `--frames` long enough to
  arrive; at chosen frames the `streaming` record shows the near districts at level 0 and the far ones at their floor, the memory and
  `gpuscene` records show packs and pool use fall after leaving a district (decision 6), and the run is repeatable: the same records at the
  same frames on two runs.
- **No holes**: every root in the `streaming` record draws a level at every recorded frame (decision 4 B: the coarser one until the finer
  is complete).
- **Hand-set and off**: `lod` on one root holds it while the camera moves; `lod-heuristic off` holds every root.
- **The rest unchanged**: the eight baseline screenshots (Sponza and three_cubes have no LOD roots) byte-identical; `assets/layered/`'s tree
  root follows the camera.
- **Trace**: one `LevelsWanted`/`LevelsSwitched`/`LevelsUnloaded` per batch, no warnings on a clean run.

## Open questions

- **What keeps the heuristic from asking for more than fits** (step 5's out-of-memory on Caldera): Caldera's district thresholds (authored
  by the converter), USD's frustum test (decision 8 A instead of B), a cap on how many roots are finer than their floor until budgets come,
  or the budgets step itself first.

- Is a level's content key (its prim index nodes) enough to share it safely when the stage's layers have opinions under it? Caldera has
  none; `assets/layered/` should get one to test it.
- Caldera's thresholds: the converter's `[0.25]` for districts and halvings for models are a starting point; the flight will show whether
  districts open too late or too early.
- Prototypes cached across composes: does any of today's per-compose state (bindings, heuristics resolved through relationships) need to
  move into the cached content?

## Deferred / follow-ups

- **Budgets, priorities, eviction** ([plan_streaming_lod.md](plan_streaming_lod.md) decisions 4 and 7). Trigger: this landed; the next
  streaming step.
- **Off-thread allocation** ([plan_async_scene.md](plan_async_scene.md), Deferred): GPU buffers and textures made on the render thread.
  Trigger: hitches during the flight above, measured by the loading frame records.
- **Layer packs ahead of meshes at the asset server** (part of [plan_streaming_lod.md](plan_streaming_lod.md) decision 7's priorities): a
  compose round's layer packs not queued behind the meshes of the round before. Trigger: the budgets and priorities step, or the flight test
  showing composition rounds waiting.
- **Each use's draws kept across rebuilds** (step 4's remaining cost: a rebuild still produces every draw, 1 to 2 microseconds each): a
  rebuild touches only the uses that changed, so a level switch removes one content's draws and adds another's. Needs prim ids and slots
  stable across rebuilds (draws, prim paths and the asset lists stored per use). Trigger: the flight test shows rebuild time is what makes
  a switch late, with many draws changing at once.
- **Distance heuristic.** Trigger: a scene that authors `LODDistanceHeuristic`.
- **Level blending.** Trigger: visible pops at switches once decision 4 B is in.
- **Cluster LOD**: under the roadmap's Rendering, deferred on purpose.
