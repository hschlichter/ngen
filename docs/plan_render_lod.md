# Render LOD: inline UsdLod levels picked per instance on the GPU

**Status. In progress.** Decisions locked 2026-10-19, each as recommended.

A UsdLod root whose levels are payloads is *streaming LOD*: which level loads is the scene thread's decision, as built by
[plan_streaming_v2.md](plan_streaming_v2.md). A root whose levels are not payloads is *render LOD*: every level is composed and held with its
content, and the renderer picks the level per instance per frame, on the GPU. This supersedes the part of [plan_streaming_lod.md](plan_streaming_lod.md)
decision 2 that says every LOD level is a payload. It lands before streaming v2 step 5, whose counting and heuristic then apply to streaming roots
only.

## Current state

Every UsdLod root goes through the streaming path (`src/world/README.md` "Loading", `src/view/README.md` "The placement tree"): the composer
composes each level as a content of its own and links it, the view places it, waits until it is ready, switches it, frees the other level's draws,
and lists the root in the `lod` and `streaming` records with what it waits for. The view's heuristic evaluates every root on the CPU when the
camera moves.

The test world ([test_streaming_world.md](test_streaming_world.md)), best-practice USD authored without the engine in mind, has two kinds of root:

| Root | Levels | Count at dense_forest | What a level is |
|---|---|---|---|
| The region `R_2_2`, its 16 tiles | `full`, `proxy`/`simplified` | 17 | a payload, a file of its own (`root.usda:90`, `R_2_2_full.usda:728`) |
| Each asset placement's `Geom` (trees, bushes, rocks, logs, structures) | `lod0`–`lod2` | 42,370 | a reference into the asset's one `*_geom.usdc` (0.07–1.4 MB, every level and variant) |

Measured after streaming v2 step 4: one camera move between viewpoints switched 5,240 roots, 2 of them tiles, built 10,530 placements and removed
87,256 slots in one pass; passes that build 60,000+ placements take 200–1,000 ms. Nothing in the asset roots' levels loads: the work is per-prop
level selection done as scene-graph edits.

The renderer already culls every draw slot on the GPU each frame (`src/renderer/README.md` "Culling and draw lists"): `InstanceCull` tests each
slot's bounds against the camera and the cascades and compacts the visible ones into indirect commands, in slot order.

## Scope

In:
- Composition: a LOD root's kind from what is authored; a render-LOD root's levels composed in place, all of them, with no level contents.
- The view: a render-LOD root where it is placed becomes a *LOD group* for the renderer; every level's draws are placed, tagged with the group and
  their level. The CPU heuristic, the switch rule, readiness and the `lod`/`streaming` rows cover streaming roots only.
- The renderer: a LOD group table, a level-selection compute pass before culling, the cull test skipping instances of levels not selected, and a
  readback of what was selected.
- Hand-set levels and the heuristic's off switch, for both kinds.
- Records, verbs, the LOD window, the Culling window and the Engine Map for render LOD.
- A render-LOD case in `assets/layered/`.

Out:
- Collapsing a content's instances of one prototype into one placement with a transform array (fewer placements per tile): measured after this
  lands (Deferred).
- Cluster or continuous LOD (meshlets): later, once a game or use case asks for it (it would revisit streaming v2 decisions 2 and 3).
- Caldera: its converter made model levels payloads ([plan_caldera_usdlod.md](plan_caldera_usdlod.md)); whether to emit them inline is part of
  revisiting Caldera, not this plan.
- Streaming roots: unchanged here; streaming v2 steps 5–7 carry on with them.
- Composition semantics: `ngen-worldcheck` against `ngen-usdcheck` stays identical.

## Decisions

### 1. The split, from what is authored

Locked (2026-10-19). A LOD root *streams* when any of its LOD items has a payload on its prim index; otherwise its levels are *render LOD*. A
payload is USD's own "load this or not" signal, so the author already said which LOD is loading and which is drawing; nothing is computed or
guessed. A root mixing payload and inline levels streams (its inline levels are contents that load at once).

### 2. How a group's levels reach the GPU

- **A. A slot per level's draw**, as today per draw: every level's submesh draws are placed, each slot tagged with its group and level. The cull
  test drops slots of levels the group didn't select. The instance record grows by the group index; draw commands, buckets, materials per submesh
  and the shadow path stay as they are.
- **B. One slot per draw with its levels' ranges** in it (mesh, index range per level): a third of the slots, but a level's submeshes and materials
  differ from another's, so a slot would carry several materials and the command build would pick per level.

**Locked: A** (2026-10-19): it is one tag and one test in what exists, and the culling pass is built to scale into the millions. The cost is slots: every level of
every placed asset instead of one (estimate for dense_forest: about 3× the asset draws, roughly 250,000 more slots, 128 bytes each on the GPU).
B is the follow-up if the instance buffer or the cull time shows in the `memory` record and the GPU zones.

### 3. Where the level is picked

- **A. A pass per group before culling** (`LodSelect`, one thread per group): screen size from the camera, the thresholds, the 20% band against
  the group's last level, written to a per-group state buffer that the cull pass reads.
- **B. In the cull pass per instance**: every instance of a group computes the same level; the band's state would be written by many threads.

**Locked: A** (2026-10-19): one computation per group, one writer per state word, and the chosen levels are one buffer to read back.

### 4. The cascades

Locked (2026-10-19): the shadow cascades draw the level the camera picked, so a prop's shadow matches the prop. Picking per cascade would cost a selection per view and
let shadows change level as the light's view moves; not needed.

### 5. Hand-set levels and the heuristic off

Locked (2026-10-19): a group carries a forced level: the root's hand-set level (`lod` verb, the LOD window), else its default while the heuristic is off, else none.
Setting one is a group change in the change list, not a placement change. Turning the heuristic off rewrites every group (42,370 records,
about 2 MB), once.

### 6. Which views the heuristic measures

Locked (2026-10-19): the camera's position, field of view and aspect, as the CPU heuristic measures (`screenSize` in `viewscene.cpp`): the same formula, ported to the
shader, so a root picks the same level on either side. The frozen culling frustum (`CullState`) doesn't change it: LOD follows the live camera.

### 7. A level not resident yet

Locked (2026-10-19): a group draws what it already has until the level it wants is resident. The cull pass, which visits every alive instance,
marks in a per-group mask the levels with an instance whose mesh isn't in the pool (its mesh table entry is empty: not copied yet, or waiting for
pool room). The next frame `LodSelect` reads and clears the mask, and picks the wanted level when it is resident, else the level it drew last
frame when that is, else the nearest resident level, coarser first. The mask lags a frame; it needs no pass of its own. A group with no resident
level draws nothing, as today.

### 8. How many levels a group holds

UsdLod sets no limit (N levels, N − 1 thresholds); the limit is the GPU record's. What is authored: the test world's assets have 2 or 3 levels;
Caldera's models after its converter 1 to 4 (5,510 with 1, 1,729 with 2, 458 with 3, 376 with 4); shipped games commonly author 3 to 5 per prop
and some engines allow 8.

- **A. Fixed, 8 levels** (7 thresholds): a 64-byte record, 2.7 MB at 42,370 groups. A root with more uses its first 8 and the packer warns.
- **B. Variable**: thresholds in a shared array, an offset and count per group. No limit, one more buffer and indirection in the shader; nothing
  authored needs it.
- **C. Fixed, 5 levels** (4 thresholds): a 48-byte record; tight against Caldera's 4.

**Locked: A** (2026-10-19): the memory is small, the shader reads one record, and the warning says when a scene needs more.

## Steps

1. **Composition** (`src/world/`).
   - `LodRoot` gains `bool streams = true;`: "a level is a payload: levels are contents of their own, linked as they load; else every level is
     composed in place and the renderer picks per instance".
   - `WorldComposer::composeChildren`, for a LOD root: the items' indexes are probed already; when none has payloads, the items are composed as
     ordinary children (`composePrim`), `LodRoot::levels` names them, `loaded` holds every level, and no `LevelSlot` is made. A stand-in and its
     `linkedContent` exist only for a streaming root.
   - `World::visit` walks a render-LOD root's levels as children; `wantedLevel`/`drawnLevel` and `lodLevels` concern streaming roots.
   - `ngen-usdcheck` loads what the World loads: render-LOD levels aren't payloads, so USD composes them anyway; its LOD counting is unchanged.
   - `assets/layered/`: an asset with a UsdLod root whose two levels are references (a `RenderLodRock`, placed three times, one an instance),
     beside the payload-levelled `Tree`.
   - `src/world/README.md`: the two kinds.
2. **The renderer** (`src/renderer/`).
   - `renderworld.h`:
     ```cpp
     // A render-LOD root where it is placed: the renderer picks its level per frame from the camera. Its draws are slots tagged with it.
     struct RenderLodGroup {
         glm::vec3 center = glm::vec3(0.0f); // the heuristic extent's bounding sphere, world metres
         float radius = 0.0f;
         std::array<float, 7> thresholds = {}; // finest first, as LodRoot::thresholds; levelCount - 1 of them used (decision 8)
         uint32_t levelCount = 0;
         uint32_t defaultLevel = 0;
         int32_t forcedLevel = -1; // hand-set, or the default while the heuristic is off
         bool alive = false;
         auto operator==(const RenderLodGroup&) const -> bool = default;
     };
     ```
     `RenderWorld::lodGroups` (`ChunkedList<RenderLodGroup>`), `RenderMeshInstance::lodGroup` (group slot + 1, 0 for none; `lodLevel` is then the
     slot's own level + 1), `SceneChanges::lodGroups` and `lodGroupCount`, applied with the draw slots.
   - `lodgroups.*` (built as its own class beside `GpuScene`): a group table (`GpuLodGroup`, std430, 64 bytes), a persistent state buffer (one `uint` per group: the level picked last frame)
     and a residency mask (one `uint` per group: a bit per level not resident), grown like the instance buffer; `GpuInstanceRecord` gains
     `lodGroup` (112 → 128 bytes).
   - `passes/lodselectpass.*`, `shaders/lodselect.comp`: one thread per group; forced level, else the screen-size level with the band against the
     state; then decision 7's fallback against the mask, which it clears; writes the state and counts groups per level, and groups drawing a level
     other than the one they want, into the totals.
   - `shaders/instancecull.comp`: an instance with a group is visible only when the group's state is its level; an instance whose mesh entry is
     empty sets its level's bit in its group's mask; the totals count the instances dropped by level (one count: a level not drawn is in
     no view), and their cull code is `D`.
   - `CullReadback`: the per-level counts always; the whole state buffer only while asked (the LOD window open or a `renderlod` record pending), as
     the commands are read only while the render debugger is open.
   - `src/renderer/README.md`: "Scene data on the GPU" (the group table and state), "Culling and draw lists" (the level test), Passes.
3. **The view** (`src/view/`).
   - The walk (`buildWalk`): a render-LOD root's level prims are walked as children; a prim item under one carries its root item's index and its
     level (`WalkItem::lodRootItem`, `lodLevel`).
   - `buildPlacement`: a render-LOD root item takes a group slot (freed with the placement, reused like a slot), its sphere from the root's extent
     and transform, its thresholds and default, and its forced level; every level's draws are placed, tagged. No `PlacedRoot`, no pending, no
     readiness for it. A LOD root in a point's prototype gets a group per point, so points get per-instance LOD (the composer's "every point
     draws its default level" message goes for render-LOD roots).
   - `evaluateLevels`, `rootsToCheck`, the `streaming` and `lod` rows, `contentReady` and `countWaiting`: streaming roots only. A hand-set level or
     the heuristic's switch on a render-LOD root path edits its groups (every placement of a prototype's root, by full path).
   - `src/view/README.md`: the placement tree's two kinds of root.
4. **Visibility pass** (below), and the plans' cross-references: streaming v2 steps 5 and 6 name streaming roots; plan_streaming_lod decision 2
   points here.

### Step 1 results (2026-10-19)

- **Composition**: `LodRoot::streams`; `composeChildren` probes a LOD root's children first and composes the levels in place when none has a
  payload; resolution skips render-LOD roots; `World::visit` walks their levels as children. The heuristic's resolution rebuilt `LodRoot` field
  by field and dropped the new flag, so every root with a heuristic streamed again in the view (25 prototypes' roots counted as levels still
  composing, and the tiles never switched); it keeps it now.
- **Classified on the test world**: 17 streaming roots (the region, its 16 tiles), 25 render-LOD roots (one per asset prototype, 2 or 3 levels),
  as authored. `assets/layered/` has `rock.usda` (two reference levels in `rock_geom.usda`), placed twice by reference and once as an instance.
- **World against USD** identical: layered (but the known inherit), the test world at `proxy` and with `T_1_1` at `full` (183,301 lines,
  82,266 instancer points), Caldera (54,124 lines), instancing counts.
- **The view in the meantime** draws a render-LOD root's default level (the test world's assets default to their coarsest), with no row and no
  CPU switching. At the four viewpoints the heuristic evaluates 17 roots, every root draws the level it wants, no warnings; only tile switches
  change slots. Resident after start: dense_forest 3.4 s, valley 4.1 s, four_tiles 5.5 s (5.2, 6.7, 9.1 after streaming v2 step 4); the longest
  frame while loading 71–120 ms (300–500). A tile switch still builds its 10,000–42,000 placements in 160–450 ms (the deferred per-prototype
  placement).
- Baselines byte-identical against step 4's commit; the layered LOD script gives the same levels.

### Step 2 results (2026-10-19)

- **Built**: `RenderLodGroup`, `RenderMeshInstance::lodGroup`, `RenderWorld::lodGroups` and `SceneChanges::lodGroups`/`lodGroupCount`;
  `GpuInstanceRecord` 128 bytes in the C++ and the seven shaders declaring it (and the capture schema); `LodGroups` (`lodgroups.*`: the
  table with its staging and serials, the state and residency buffers, per-slot counters, totals and readback); `LodSelectPass` with
  `shaders/lodselect.comp` (select and sum modes, as the culling's cull and scan); the cull shader's level test, residency marking,
  `counterLodDropped` and cull code `D` (the GPU Scene window counts it); `CullResult`'s render-LOD fields; `RenderSnapshot::readLodStates`.
- **Verified**: the eight baselines byte-identical against step 4's commit with validation on, no validation messages; `lodselect.pipeline`
  is created at start-up and the lit path waits for it. With no groups the frame leaves `LodSelect`, `LodSelectSum` and `LodReadback` out.
- **Not yet exercised**: nothing makes groups until step 3, so the pick, the fallback and the level test have run on no data. Step 3's
  verification (the layered rock framed near and far, `renderlod {check: true}` on the test world, the fallback flight) covers them.

## Visibility

- **`renderlod` record** (new): groups alive, groups forced (hand-set, heuristic off), instances per level placed, and from the latest readback the
  groups per level picked, the groups drawing a fallback level while the wanted one uploads, and the instances dropped by level per view; with `{path}`, the groups placed under a path with their sphere, thresholds,
  forced level and the level picked; with `{check: true}`, the level the CPU's formula picks for every group at the same camera and how many differ
  from the GPU's.
- **`lod` and `streaming` records**: streaming roots only, each with `kind`; totals gain `renderLodGroups`.
- **`culling` record and the Culling window**: instances dropped by level, per view, beside those culled by the frustum.
- **`memory` record**: the group table and state buffer by debug name (`gpuscene.lodgroups`, `gpuscene.lodstate`).
- **Verbs and RPC**: `lod <path> <level>` and `lod-reset` act on either kind; `lod-heuristic` sets every group's forced level; `record renderlod`.
- **LOD window**: the streaming roots as today; a "Render LOD" section with the group count, the histogram of picked levels, and a level
  picker for the groups under the selection.
- **`lodlevel` debug view**: unchanged; a render-LOD draw's tag is its own level, so it shows the GPU's pick.
- **Engine Map**: "LOD select" in the render thread's lane between the GPU scene and culling, with an edge from the scene thread's Placement
  ("LOD groups"); "LOD selection" (the CPU heuristic) says it covers streaming roots.
- **Trace**: none per frame; `PlacementsChanged` gains the groups added and removed.

## Verification

- **Composition**: `ngen-worldcheck` and `ngen-usdcheck` identical on `assets/layered/` (with the new render-LOD asset), the test world with a tile
  at `full` (183,301 lines today, every asset level now in both), and Caldera; `--instancing` and `--instancers` identical.
- **Baselines**: the eight screenshots byte-identical (no LOD in three_cubes or Sponza).
- **Test world, the four viewpoints and the flight**: `LevelsWanted` evaluates 17 roots; a camera move that switches no tile traces no
  `PlacementsChanged`; a tile switch's change list is that tile's two levels' draws; `renderlod {check: true}` reports 0 differences outside
  screen sizes within 1e-4 of a threshold; resident times no worse than step 4's (dense_forest 5.2 s, valley 6.7 s, four_tiles 9.1 s).
- **Layered**: the render-LOD rock picks its finer level framed close and its coarser one framed far (`renderlod {path}`), the hand-set level
  holds with the camera moving, and `lod-heuristic off` gives its default; the `Tree` (payload levels) behaves as before (`h1.txt` levels).
- **Fallback**: with a small upload budget (`view.gpuscene.set`), flying from overlook to dense_forest, `renderlod` counts groups drawing a
  fallback level while finer ones upload, and no group that drew a level draws nothing (instances dropped by level never exceed the group's
  other levels' instances).
- **Cost, measured and recorded**: the instance buffer size and the `InstanceCull`/`LodSelect` GPU zones at dense_forest before and after, against
  decision 2's estimate.

## Open questions

None open.

## Deferred / follow-ups

- **One placement per prototype per content**, a transform array like a PointInstancer's: a tile's 12,310 instance placements become tens.
  Trigger: tile switches still above 100 ms after this lands.
- **Decision 2 B** (one slot with its levels' ranges). Trigger: the instance buffer or `InstanceCull` time shows in the records at the test world's
  densest viewpoint.
- **Composer per-path level state** (`lodLevels`, `prototypeLevels`, the view's `composerLevels`): with render LOD out of it, it serves 17 roots;
  whether it moves to the placements is part of streaming v2's revisit of its decisions.
