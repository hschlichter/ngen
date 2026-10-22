# Streaming v2: simpler, for a world authored to stream

**Status. In progress.** A review of the streaming and LOD system as built by [plan_streaming_districts.md](plan_streaming_districts.md), with what
to remove and what to replace. Decisions 1 to 8 locked 2026-10-15, each as recommended; the test world is best-practice USD with variant
sets and `PointInstancer` (2026-10-16). Steps 1 to 3 done 2026-10-16, step 4 2026-10-18, step 5 2026-10-23, step 6 2026-10-23.

## Current state

What exists, and why it is shaped that way. Most of the complexity answers something about Caldera, not something streaming needs.

- **Composition** (`src/world/worldcomposer.*`, 1,450 lines): composes the stage every round. Prototypes and *linked contents* (a LOD level,
  or any prim with payloads) are kept in two `std::deque`s at stable indices. A content composed while a pack it names is missing has a *gap*,
  is recomposed in place when reached again, and carries `version` and `complete`. Layer stacks are kept and rebuilt in place, because kept
  contents point into them. `relink()` re-resolves levels without composing the stage, and `evict()` empties contents not reached for a
  while, with special cases for "reached by the latest resolve".
- **Extract** (`src/view/viewscene.*`, 2,600 lines): every extract replays the whole World from kept walks (`ExtractItem`, `extractOf`,
  `replay`), rebuilds every draw (266,330 for `map_tile_p`, 1.3 s), and diffs the result against the last one. Extracts are paced while
  assets arrive, because each costs that much. A `TransformIndex` answers transform queries.
- **Readiness**: `contentReady` walks a level's content recursively, memoised per extract, to decide whether the finer level can be drawn.
  `countWaiting` walks it again for the `streaming` record.
- **The heuristic** (`evaluateLevels`): screen size, a 20% band, hand-set levels, a global off. Then a *cap* (2 roots finer), with a ranking
  that puts roots whose extent contains the camera first, smallest first, and skips empty extents with a warning.
- **Unloading**: three mechanisms. Library and GPU entries per asset with `unusedSince` timers; mesh packs released when no loaded level
  names them (`meshesLoaded`, a full replay to compute); composer eviction by last-reached time.
- **Asset requests**: `requestDrawnAssets` replays the whole World with every loaded level to list what to fetch.

How Caldera shaped it:

| Mechanism | The Caldera problem it answers | Goes in step |
|---|---|---|
| Gaps and in-place recompose | Draw as packs arrive (plan_streaming_lod decision 6), since revised: levels now switch only when complete | 3 |
| A prim with payloads as its own content (districts step 4b) | 159,393 payload prims in one district level; recomposing the level per arrival was too slow | 3 |
| Paced extracts | A full extract takes 1–3 s with 266,000 draws | 4 |
| The cap and its ranking | Districts kilometres across switching to 200,000+ draws each; out of memory at 35 roots finer | 2 |
| Camera-inside ranking, empty-extent skip | Screen size is meaningless inside a kilometre-wide extent; the terrain sections author an empty box | 2 |
| `TransformIndex` | A map of every prim's transform cost too much at Caldera's prim count | 4 |
| Composer eviction special cases | Contents unreached for seconds while 50,000 mesh packs arrive | 5 |

Each row is removed outright in its step, its records, verbs, RPC parameters, window controls and README sections with it; nothing is kept
behind a flag "for Caldera". Caldera is revisited afterwards against what remains (Deferred).

[test_streaming_world.md](test_streaming_world.md) describes a world authored for streaming: 1 km regions, 250 m tiles and asset roots, each
level one file, every switch bounded (a tile `full` level under 20,000 instances). With that world as the target, most of the table goes away.

## Scope

In:
- The runtime model: a tree of placements of immutable contents, replacing whole-world extract, replay and diff.
- Composition: contents exist only when complete; one content store; only LOD levels and prototypes are contents.
- Readiness and unloading by counting: pending counts and reference counts, one delay.
- Removing the cap, the camera-inside ranking, extract pacing, `TransformIndex` and the composer's time-based eviction.
- The test world's representative area (test_streaming_world.md, section 8 step 3) as the scene this is built and verified on.

Out:
- Budgets, priorities and prefetch (plan_streaming_lod decisions 1's margin, 4 and 7's priorities): the next step, on top of this.
- Texture streaming by mip level.
- Caldera: revisited after this lands, by changing either the code or the authored structure (a converter change in the fork).
- Meshlets and occlusion culling (the roadmap's next vertical slice step); this plan keeps their seam in mind (decision 1).
- Changes to composition semantics: inside a content, composition stays exactly as now, and `ngen-worldcheck` against `ngen-usdcheck` stays
  identical.

## What stays

From [plan_streaming_lod.md](plan_streaming_lod.md): decision 1 C (one system: a wanted level per root, the coarsest level loaded as the floor),
decision 2 (UsdLod is the only LOD; a level is a payload), decision 3 (the structure is authored), decision 7 (requests are never cancelled).
From [plan_streaming_districts.md](plan_streaming_districts.md): decision 4 B (switch when the finer level is complete), 5 A (the band), 6 A
(unload after a delay; never the floor), 7 A (hand-set levels win), 8 B (no frustum in the heuristic). These were good decisions; v2 changes how
they are built, not what they say.

## Decisions

### 1. The runtime model: placements of contents

Today the World is walked as a whole and every draw is rebuilt per extract. A content (a level, a prototype) never changes once composed, so its
draws don't either: only where it is placed does.

- **A. Keep the whole-world extract**, and do the deferred "draws kept across rebuilds" on top: diff per use. Keeps the replay, the kept walks
  and the diff, and adds stable slots across them.
- **B. A tree of placements.** A *placement* is a content at a world transform with the state it inherits (purpose, material binding). A
  content's draws are built once, in its own space, when it completes. A placement owns a range of instance slots: its content's draws times its
  transform. A LOD root inside a content is a child placement per root use, pointing at the level it draws; an instance is a child placement of its
  prototype. Switching a level removes one child placement's slots and adds another's; nothing else is touched.
- **C. GPU-driven placement**: the GPU expands placements into instances. Right eventually (meshlets), premature now.

**Locked: B** (2026-10-15). It is simpler than A, not an addition to it: the replay, `ExtractItem`, kept walks, the diff, extract pacing and
`TransformIndex` all go, replaced by one tree whose edits are the change lists. Queries (`transform`, `bounds`, picking paths) walk the placement
tree. It is also the seam meshlets want: a content's meshlet list per placement.

Sketch (`src/view/placement.h`, new):

```cpp
// A content placed in the world: its draws, in its own space, at a transform.
struct Placement {
    int32_t content = -1;            // in World::contents
    glm::mat4 world = glm::mat4(1.0f);
    Purpose purpose = Purpose::Default;
    std::string material;            // inherited binding, when strongerThanDescendants
    std::string path;                // full path of the content's root prim
    uint32_t firstSlot = 0;          // instance slots this placement owns
    uint32_t slotCount = 0;
    std::vector<PlacedRoot> roots;   // LOD roots in the content, one per use
    std::vector<uint32_t> instances; // child placements of prototypes
};

// A LOD root where a placement uses it: the level it wants, the level it draws, and the placements of its loaded levels.
struct PlacedRoot {
    EntityId root = noEntity;
    uint32_t wanted = 0;
    uint32_t drawn = 0;
    std::vector<std::optional<uint32_t>> levels; // a child placement per loaded level, by level index
    std::chrono::steady_clock::time_point unwantedSince;
};
```

Only the drawn level's placement owns slots; a loaded, undrawn level (the floor while a finer one is drawn, a finer one still loading) is held
without slots.

### 2. A content exists only when complete

Gaps exist so that a level can be drawn while it arrives. Since decision 4 B, nothing draws an incomplete level, so a partial content has no
reader.

- **A. Keep gaps and in-place recompose.**
- **B. Compose a content in rounds, keep it only when complete.** A round that finds missing packs returns them and discards the partial
  result; the next round composes it again. Once complete, it is stored and never changes (until a pack it read changes: drop it, as today).

**Locked: B** (2026-10-15). It removes `complete`, gap tracking, recomposing in place, and rebuilding layer stacks in place (a stack is kept once complete). The
cost is composing a content once per round of arcs, which is bounded when a level is one tile (a few rounds of a few thousand prims), and was not
for a Caldera district.

### 3. Only LOD levels and prototypes are contents

Step 4b made every prim with payloads a content of its own, for Caldera's 159,393 payload prims per district.

- **A. Keep payload prims as contents.**
- **B. A payload that isn't a LOD level is part of its parent's content**, loaded with it, as USD loads it.

**Locked: B** (2026-10-15): one fewer kind of content and of link, and the payload flag on level slots goes. A world that puts thousands of payloads
in one level pays for it in composition rounds; that is an authoring choice (test_streaming_world.md 9.2), and Caldera's case for the revisit.

`World::prototypes` and `World::linkedContents` become one `World::contents`; a stand-in entity links to a content by index, as `linkedContent`
does now.

### 4. Readiness by counting

- **A. Keep `contentReady`**: a recursive walk, memoised per extract, and a second walk for the record.
- **B. Pending counts.** When a content completes, it lists what it needs: its meshes, materials and textures, and the floors of the LOD roots in
  it. A placed level is *ready* when that list is held. Arrivals decrement; the record reads the count.

**Locked: B** (2026-10-15). One number per level, updated when something arrives, read by the switch rule and shown as is ("waits for 312 meshes"). The asset
requests come from the same list (decision 6).

### 5. Unloading by reference count

- **A. Keep the three mechanisms.**
- **B. One rule.** A level that isn't wanted, isn't drawn and isn't the floor, for the unload delay, has its placement released. A content is
  held while a placement uses it, an asset while a held content lists it; at zero they are released at once. The delay lives in one place, on
  the level.

**Locked: B** (2026-10-15). Released at once, because the delay already happened on the level. Turning around within the delay keeps everything, as now. The
composer's `evict`, the `unusedSince` maps, `meshesLoaded` and the "reached by the latest resolve" rule go.

### 6. Requests per content

- **A. Keep `requestDrawnAssets`**: a whole-World replay with every loaded level, after each batch.
- **B. A content's list (decision 4) is requested when it completes**; a level wanted is requested when the root changes its wanted level.

**Locked: B** (2026-10-15), which follows from 4.

### 7. The cap

- **A. Keep the cap**, with its ranking.
- **B. Remove it.** A world authored with bounded levels doesn't need it; memory limits are the budgets step's (plan_streaming_lod decision 4),
  which decides by priority and memory, not by a root count.
- **C. Keep a plain cap** (no ranking, screen size order) as a safety valve until budgets.

**Locked: B** (2026-10-15). The cap exists because Caldera districts switch 200,000 draws at once; the ranking exists because screen size from inside a
kilometre-wide extent means nothing. On the test world neither holds. Caldera without a cap runs out of memory at its first evaluation near
the island: accepted until it is revisited, with the heuristic off for it in the meantime. It goes first, in step 2, with everything around
it.

### 8. The heuristic

Locked (2026-10-15). Stays: screen size of the extent's bounding sphere, the 20% band, hand-set levels, the global off, no frustum. Changes:
- Evaluated over the placed roots (decision 1), which with nested cells are only the roots near the camera.
- An extent that is empty or not finite is a packer warning naming the prim; the root keeps its default, with no runtime ranking around it.

## Steps

1. **The test world's representative area.** Run test_streaming_world.md (its run copy in `~/Code/streamtest-world/`, a project of its own
   that knows nothing of ngen) through its section 8 step 4 (one region, at the full hierarchy depth); its `world/` is served as
   `assets/streamtest-world/world/` (a link to the project). Then see what today's engine makes of it: the packer's warnings (variant sets
   and `PointInstancer` are authored, and neither is read today), `ngen-worldcheck` against `ngen-usdcheck` where the engine reads the scene,
   and a flight as the baseline. What the engine doesn't read is engine work, never a change to the world.
   **Variant sets and `PointInstancer`** (decided 2026-10-16: the test world is best-practice USD and uses both; the engine adapts) are a plan
   of their own, landed before step 4, so the placement tree is built with them: variant selection is composition (the
   composer, `ngen-usdcheck` parity), point instancing is placements of a prototype by the thousand (one placement per instancer, its slots per
   point). Today the packer warns about variants and point instances aren't drawn.
2. **Remove the cap** (decision 7), first, because it depends on nothing else and the test world needs it gone (it runs with
   `lod-cap 64` until then, and still caps 741 roots at one viewpoint). Removed: the cap and `heuristicCap` in `evaluateLevels`, the ranking
   (`aroundCamera`, the camera-inside order), the runtime extent check (`validExtent`, `warnedExtents`, `LodExtentEmpty`; the packer warns
   about an empty or non-finite heuristic extent instead, and the root keeps its default), `lodCapped` and the "capped" reason, `setLodCap` and
   `lodCap`, the `lod-cap` verb and `view.lod.heuristic {cap}`, `cap` and `capped` in the `streaming` record, the LOD window's Cap field
   (`LodWindowState::cap`, `capRequested`) and its wiring in `view.cpp`, "capped" in the Engine Map's LOD selection node, and the view
   README's "The cap". Verified: `rg -w -i "cap|capped|lodCap|aroundCamera|validExtent|LodExtentEmpty"` over `src/` finds none of them; the
   test world at its viewpoints with no `lod-cap` line; baselines byte-identical.
3. **One content store, complete contents only** (decisions 2, 3): `World::contents`; `WorldComposer` links a content only when complete and
   drops payload contents. `evict` stays until step 5, whose reference counts replace it (unloading needs either one). `ngen-worldcheck`
   against `ngen-usdcheck` identical on the layered scene, the test area and Caldera.
4. **Placements** (decisions 1, 6): `src/view/placement.*`; the scene thread keeps the placement tree and turns its edits into change lists;
   queries walk it. A content's walk stays, as its draws' template, built once per content version and inherited state (purpose, material
   binding); what goes is the whole-World replay, the slot diff (`slotOfDraw`), extract pacing, `TransformIndex` and the per-extract
   `readiness`. Locked 2026-10-16:
   - **What the main thread reads (A1)**: the snapshot keeps a flat `RenderWorld` and its tables, copied when the tree changed (building is
     proportional to the change, the copy isn't: about 6 MB on the test world). The placement tree is published as immutable shared nodes,
     path-copied when edited, and `transform`, `bounds` and the records walk it. Copy-on-write slot chunks (A2) if the copy shows in the
     `zones` record.
   - **Drawing while meshes arrive (C1)**: a placement draws what is held and is filled in as assets arrive, through an index from asset to
     the placements waiting for it, so a scene opens progressively as today; the same lists are decision 6's requests.
   - **Edits**: a compose re-places contents whose version changed, reusing child placements whose path, content, transform and state are
     unchanged; a level switch removes one level's slots and adds another's; an arrival updates the placements waiting for it; a gizmo
     opinion gives its placement a private template; a purpose toggle or a repack rebuilds the tree.
   - Until step 5, the library's used sets come from the live slots (`collectChanges` and its timers stay), and the switch rule's readiness
     from the templates.

   In four parts, each observable: **4a** the tree replaces replay and diff for drawing (`WorldExtracted` becomes `PlacementsChanged`, with
   the slots added and removed per edit); **4b** queries and records read the tree, `TransformIndex` goes; **4c** requests per content,
   `requestDrawnAssets` and `meshesLoaded` from the placements; **4d** visibility (`scenethread` record, Scene Flow, the Engine Map's
   "Extract" becomes "Placement").
   **Before step 5: [plan_render_lod.md](plan_render_lod.md)** (2026-10-19). Step 4's measurements showed the volume is per-prop LOD: of 42,387
   roots at dense_forest, 17 have payload levels; the rest are assets whose levels are inline references. Those become render LOD, picked per
   instance on the GPU, and steps 5 and 6 below apply to streaming roots (payload levels) only.
5. **Counting** (decisions 4, 5): pending counts per placed level for the switch rule; reference counts for contents and assets; the unload
   delay on the level only. Locked 2026-10-22, each as recommended:
   - **The delay is the view's (F1).** A level that stops being wanted stays loaded for the unload delay, then goes; one still drawn when
     its delay runs out (the root waits for the level it wants) is kept for another. The view keeps a time per root and level and gives the
     composer the levels to keep besides the one wanted (`WorldComposer::keepLodLevels`); the composer has no clock.
   - **Contents are released when no resolve reaches them (F2)**: after each compose and relink, a kept content the resolve didn't reach,
     and that isn't being composed (or used by one that is), is emptied at once. Resolve already walks every held content, so this is the
     reference count without counts to keep in step. `evict`, `lastReached` and the "reached by the latest resolve" rule go.
   - **Library and GPU entries are held while a placement uses them, drawn or not (F3 a)**: a loaded level not drawn (the floor under a
     finer one, a level in its delay) holds its meshes' and materials' entries, so switching to it, finer or coarser, draws at once. The
     unused-since timers go.
   - **A level is ready when its placement waits for nothing (F4)**: its meshes, material packs and textures, the instances in it, and the
     floors of the LOD roots in it, counted per placement as `waiting` already is and summed up the tree. A finer level now switches in
     with its materials. The level placements are synced before the root picks its level, so it switches in the pass the last piece
     arrives. `contentReady`, `itemsReady`, `countWaiting` and the per-pass `readiness` go.
6. **The heuristic over placed roots** (decision 8).
7. **Visibility pass and docs**: `src/world/README.md`, `src/view/README.md`, the Engine Map.

### Step 1 results (2026-10-16)

- **The world**: Eldermark, seed 1337, region `R_2_2` generated (1 km, 16 tiles, each tile `full` and `simplified` a `.usdc` of its own, the
  region `full` a `.usda` and `proxy` a `.usdc`), with 8 named viewpoints and a flight path. A tile `full` holds 12,310 instanceable asset
  placements and 4 `PointInstancer`s. The assets put their geometry in a `shape` variant set (`juniper`, `shrub`, `deadfall` and others), with a
  UsdLod root on `Geom`.
- **LOD didn't switch at first**: three places where the engine read UsdLod differently from the spec, fixed (not part of this plan's
  steps): a root's heuristic is now the first of a type and domain the engine reads (it took the first target, here a distance heuristic, and
  kept every region at `proxy`); a heuristic's `boundingVolume` bounds the root; a LOD level's own authored visibility is ignored for the
  level drawn (the world marks every non-default level invisible, as UsdLod recommends for readers without LOD). `src/world/README.md` has
  the rules.
- **After the fixes**, at `dense_forest`: `R_2_2` measures 9.4 against its threshold of 1.107 and goes `full`; its tiles go `full` or stay
  `simplified` by their own size; nothing waits. Terrain, rocks and trees draw; the terrain draws white (its material isn't read; not looked
  into yet).
- **42,387 LOD roots** at that viewpoint, 39,891 of them asset roots at their coarsest level, one per placement in the tiles at `full`. With
  `lod-cap 64` the cap held 741 back (step 2). Every evaluation walks them all: step 6's cost is per placed root, and this is its measure.
- **Composition**: `ngen-worldcheck` against `ngen-usdcheck` identical with the region at `proxy` (70 prims) and at `full` (567; its tiles at
  `simplified`). With a tile at `full`, 91,516 of 140,789 lines differ, all under the variant assets' `Geom/lod*` (the geometry the variant
  brings), and USD has 25 prototypes to the engine's 9 (variant selections make prototypes of their own).
- **What the engine doesn't read yet**, for the variants and `PointInstancer` plan: variant sets (a packer warning per placement that selects
  one, so per asset placement in a tile), `PointInstancer` (not drawn), and LOD items that are `over`s (the assets' levels outside the variant):
  UsdLod counts only defined children as items, the engine counts every child.
- **The baseline flight** (after step 2, so not a capped one): the 27 `flight_NN` cameras, `wait-resident` at each, records on arrival
  and 1,500 frames later, `lod-unload-delay 1` (the view runs at about 470 frames a second here, so a frame count says little about time:
  the first try, with the default 10 s delay and 700 frames, ended before anything was due). The path crosses `R_2_2` from cp08 to cp20,
  the only region generated. Every root draws a level at all 54 records; up to 7 of 16 tiles `full` and 57,224 roots (cp15); mesh packs
  rise to 99 inside the region and fall back to the region proxy's 4 a second after leaving it (107 released by cp21); peak resident
  memory 1.2 GB; no view warnings. Two runs give the same levels for every root at every record; the held packs differ at 2 arrivals (99
  against 91, 91 against 97), as releases follow the clock, and match once settled.
- **`wait-resident` answered before a moved camera's levels**, so each arrival recorded the previous checkpoint: `ViewScene::idle()` didn't
  count a camera posted and not yet evaluated, nor a batch taken and not yet published. Both count now (`src/view/README.md`, "Resident"):
  arrivals match their settled records. While the camera moves the scene isn't resident, and `SceneResident` follows each stop.

### Step 2 results (2026-10-16)

- **Removed**: the cap, its ranking and the runtime extent check, with every verb, RPC parameter, record field, window control, Engine Map
  line and README section listed in the step. `rg` over `src/` finds none of the names (the remaining matches are a cone's base cap and the
  build scheduler's).
- **A heuristic without usable bounds**, which the view's runtime check used to catch, is now the composer's: it doesn't choose a screen-size
  heuristic whose bounds are empty or not finite and tries the next target, as UsdLod has a renderer ignore a heuristic with no usable result;
  a root with none keeps its default (`LodRoot::heuristicChosen`, "no usable heuristic" in the records, `chosen` in the `lod` record). The
  packer warns about the authored extent (usd rule version 4): on Caldera's default levels, 5 vehicle assets' heuristics.
- **Test world, no `lod-cap`**: at the overlook the region stays at `proxy`; at `dense_forest` and `valley` it is `full` with 4 of 16 tiles
  `full`, at `four_tiles` 6; every root's level is the heuristic's (42,387 and 46,006 roots), no view warnings, peak resident memory 1.0 GB.
  Draws are few (209 slots at `dense_forest`) because the asset roots' levels are empty until variants compose.
- Baselines byte-identical; the layered tree follows the camera, holds a hand-set level and its default with the heuristic off; World
  against USD unchanged.

### Step 3 results (2026-10-16)

- **One store**: `World::prototypes` and `World::linkedContents` are `World::contents`, each content with a `kind` (`Prototype`, `Level`);
  the composer keeps one `ContentInfo` beside each. `WorldContent::complete` is gone.
- **Complete contents only, at the links**: an instance's `prototype` and a level stand-in's `linkedContent` are set only once the content
  is complete, -1 until then. An instance whose prototype isn't complete is a gap in the content holding it, so that content isn't linked
  either. The view's readiness (`contentReady`) no longer checks completeness: what is linked is complete. A wanted level not linked yet
  counts as one content waiting (`waiting.contentsIncomplete`, the LOD window's tooltip).
- **Kept from decision 2 B, as a deviation**: composing a content in place while it has gaps, and rebuilding a layer stack in place. Both are
  internal to the composer now (`ContentInfo::complete`, the gap windows), and nothing reads a partial content; discarding a partial result
  and recomposing it would cost the same rounds. Revisit if step 4's placements want contents immutable once stored.
- **No payload contents**: a prim with payloads that isn't a LOD level composes in its parent's content (`LevelSlot::payload`, the payload
  branch in `composeChildren`, the payload loop in resolution, and the view's payload stand-in items are gone).
- **A binding exposed by linking late**: a level inside a prototype names its prototype's `Materials` at the path of the instance the
  prototype was composed for, which the composer found through the stage. With that instance's tile not linked until complete, a level
  reached through another tile's instance warned "material binding … isn't there" (6 warnings at the test world's viewpoints). Such
  targets are now looked up in the prototype (`locateOutside`).
- **Verified**: World against USD identical on the layered scene (but `/World/Inherited`, an inherit, which the packer reports), the test
  world at `proxy` (70 lines) and with `T_1_1` at `full` (183,301 lines, instancing counts, 82,266 instancer points), and Caldera (54,124
  lines, 57 prototypes); the composer's messages unchanged on all three. Baselines byte-identical. Test world at overlook, dense_forest,
  valley and four_tiles: every root draws the level it wants, nothing waits, no warnings. The layered LOD script gives the same levels as
  before.
- `viewscene.cpp` 2,135 to 2,116 lines, `worldcomposer.cpp` 1,875 to 1,860.

### Step 4 results (2026-10-18)

4a to 4d landed together: the tree is what the requests, queries and records read, so none of them could keep the replay.

- **The tree** (`src/view/placement.*`): `Placement` nodes (the stage, a prototype at an instance, each loaded level at its root, a
  `PointInstancer` as one placement with a segment per shown point), shared with the published snapshots and copied before a change while a
  snapshot holds them. Gone: `replay`, `ExtractItem` (now `WalkItem`, the content walk kept as the draws' template), the slot diff, extract
  pacing, `TransformIndex`, `requestDrawnAssets`, `meshesLoaded`, `RenderWorld::primInstances`, `WorldExtracted` (now `PlacementsChanged`).
- **A1 became A2**, as the plan's trigger said: on the test world's tile switch, copying the slots, prim paths and per-prim assets for the
  snapshot took about 220 ms a pass. The three lists are `ChunkedList`s (`src/renderer/renderworld.h`): a copy shares chunks, a write copies
  the chunk a copy still holds. Readers of `meshInstances` are unchanged but `sceneBoundsOf`, which now runs per chunk.
- **Passes look only where something waits or changed.** The first version walked every placement each pass (about 300 ms for one root
  switching, 160,000 placements). A placement now counts what it and its subtree wait for (`pending`: assets not held, roots not drawing the
  level they want or with a loaded level not linked yet); a pass goes into a subtree only when that count isn't 0, a root under it changed
  its wanted level (`rootsToCheck`), a gizmo moved something under it, or a placed content was composed again (then everywhere). A placement
  built again puts its draws back in its own slots in order, so an unchanged draw sends nothing; the first version freed and re-took them
  from the shared free list, shuffling slots between placements (166,390 "updated" for 2,191 small switches). Library use, mesh pack names,
  lights and placement counts are kept as counts; the records are gathered from the published tree on their first query.
- **Measured, test world, the four viewpoints**: every root draws the level it wants, the same levels, tiles and frustum counts as step 3,
  no warnings. One root switching: 1.6–5.6 ms a pass (300 ms before the pruning). A tile switching: the change list is the two levels'
  draws (`T_2_3` 1 → 0: 69,421 slots added, 4 removed). Resident at four_tiles 9.1 s after start against 20.0 s in step 3 (valley 6.7 s
  against 8.4 s, dense_forest 5.2 s against 4.5 s). Passes that build 60,000+ placements still take 200–1,000 ms: a tile's 12,000 instances
  each built when it is drawn, and every drawn instance built again when a material it draws with arrives (C1).
- **Baselines** byte-identical. The render record's slot numbers and library handles now follow arrival order (a prim takes its slot when its
  mesh arrives); its counts and mesh list are the same, but for 2 fewer stand-in materials kept for the unload delay (55 against 57).
- **Layered**: the LOD script gives the same levels; its record taken in the frame a level was wanted now shows the finer level already
  loading (a pass runs in the batch that relinks). Gizmo moves checked by framing: a prim (+5 y), an instance (+3 y), a prim inside an
  instance (+1 x) move; the other instance of the same prototype doesn't.
- **Caldera** with the heuristic off: resident in 4.9 s (4,427 draws, 268 placements), no warnings. Its stage is large and composed again
  every round, so each pass rebuilds the stage's own draws (14 placements built in 534 ms): open question 2.
- **Visibility**: `scenethread`'s `extract` is now `placement` (passes, needed, placements and drawn, walks built and kept, the last
  changing pass's time, built, slots added, removed, updated); Scene Flow shows the same; the Engine Map's "Extract" is "Placement";
  `PlacementsChanged` per pass. Gap: no record or RPC method answers a prim's transform (`ViewScene::transform`); the check above went
  through `camera-frame`.
- **Lines**: `viewscene.cpp` 2,116 → 1,164, `placement.*` 1,632 new; with `worldcomposer.cpp` (1,860) 4,656 against 3,976.
- **Follow-ups seen**: the main thread rebuilds the LOD window's 42,000 rows on every snapshot (`SceneUpdate`, up to 120 ms), before this
  step too; a material arriving rebuilds every drawn instance using it, where updating its slots' material handle would do.

### Step 5 results (2026-10-23)

- **Counts**: each placement counts what it waits for by kind (`PlacementWait`: meshes, material packs, textures, levels composing) as it
  is built, summed up the tree with each LOD root by its coarsest level; a root draws the finest loaded level not finer than the one it
  wants whose placement counts 0. The level placements are synced before the root picks, so it switches in the pass the last piece arrives.
  Gone: `contentReady`, `itemsReady`, `countWaiting`, the per-pass `readiness`. The record's and the LOD window's `waiting` is the wanted
  level's count (`meshes`, `materials`, `textures`, `composing`; `meshesInFlight` went with `countWaiting`).
- **The delay on the level**: the view keeps a time per root and level not wanted (`levelsKept`), gives the composer the set to keep
  (`WorldComposer::keepLodLevels`, beside `setLodLevel`), and wakes when the next runs out; a level its root still draws then gets another
  delay. Gone: the three unused-since timers, `releaseDue`, `libraryNeeded`, `lastEvict`, `unload()`.
- **Contents**: `releaseUnreached` after each compose and relink empties what the resolve didn't reach, holding what is being composed and
  the prototypes it uses (without that, a level waiting for packs would compose its prototypes again every round). A released content has
  no entities, so it doesn't make the next pass look at every placement. Gone: `evict`, `lastReached`, `lastResolveAt`, `Stats::evicted`.
  On the test world the composer releases the unloaded variant of a level as its loaded one replaces it (a level is linked loaded or not).
- **Library entries held by undrawn placements**: `heldMeshes`, `heldMaterials`, counted with the slots' uses; a placement switched away
  from gives up its slots and holds their entries without being built again (`undrawPlacement`). Without that fast path, the pass switching
  four tiles coarser built 70,811 placements again (890 ms); with it, 11 (425 ms).
- **Measured, the four viewpoints** (against step 4 with the flat bookkeeping): the same draws resident at each (59, 82,053, 826,719,
  286,626, 509,482), no warnings. Placement passes 5.59 s in all against 5.15 s, placements built 255,565 against 253,615; slots updated
  238 against 82,349, as no level switches in with stand-in materials now. The pass removing 826,660 slots takes 425 ms against 315 ms (it
  now keeps the finer levels for the delay, holding their entries). An undrawn level's placement resolves its draws (to hold their entries),
  which is where the rest of the difference goes.
- **The 27-camera flight, delay 1 s**: every root draws the level it wants at all 54 records, and the levels per root match the commit
  before at every record; levels, contents and mesh packs are released behind the camera (by `flight_26`: 129 levels, 1,288 contents, 1,552
  mesh packs), held mesh packs follow it (266 at the end), library entries equal the packs held. No warnings.
- **Layered**: the LOD script gives the same wanted and drawn levels; after going coarser the finer level stays loaded for the delay (the
  `streaming` record's `kept`, 10 s left). Composition unchanged: World against USD identical on the test world (at `proxy`, with `T_1_1`
  at `full`, its instancers) and Caldera; the layered scene but `/World/Inherited`, as before. Baselines byte-identical.
- **Visibility**: `streaming` (`waiting` by kind per root and in the totals, `kept` with seconds left, `unloaded` levels, contents, mesh
  packs, `keptLevels`), `scenethread` (`unloaded` the same), the LOD window (the loaded column's tooltip: kept levels and seconds left; the
  loading tooltip by kind), Scene Flow (kept and unloaded), `LevelsUnloaded` (levels, contents, meshes), the Engine Map's LOD selection and
  Placement text. `lod-unload-delay` and `view.lod.heuristic {unloadDelay}` now set the level's delay.
- **Lines**: `viewscene.cpp` 1,195 → 1,267, `placement.*` 1,916 → 1,968, `worldcomposer.cpp` 1,892 → 1,917.
- **Unchanged**: levels set inside prototypes still never unload (`prototypeLevels` only grows); the test world has no streaming roots in
  prototypes. Material and texture packs are never released, as before.

### Step 6 results (2026-10-23)

- Step 4 had already moved the evaluation onto the placed roots: it read the streaming roots of drawn placements from the records (32 to
  112 on the 27-camera flight, against 42,387 roots when step 1 measured). What was left was how it got them: the records are gathered by
  walking the whole tree, 26 ms at the median and 40 ms at most per evaluation, against 0.1 ms for the roots themselves.
- **The placed roots are kept as placements change** (`ViewScene::placedRoots`): a drawn placement's listed roots are added and removed with
  its uses (`useNode`), and a row the pass changes is updated with it. The evaluation and the unload delay's drawn check read that.
- **Measured**: evaluation 0.18 ms at the median, 0.62 ms at most, on the same flight; every root's levels the same as step 5 at all 54
  records, the same levels, contents and mesh packs unloaded, no warnings. The layered LOD script gives the same levels; baselines
  byte-identical.
- **Visibility**: none added; the `streaming` record's `evaluation.ms` shows the change.

## Visibility

- **`streaming` record**: per placed root its screen size, in frustum, wanted, drawn, loaded levels and, per loaded level, its pending count;
  totals: placements, contents held, assets held, pending. `cap` and its ranking fields go.
- **`scenethread` record and Scene Flow**: contents composed and held (one store), placements, slots; releases by kind. Extract timings become
  placement timings (build a content's draws, place, release).
- **LOD window**: unchanged rows; the Cap field goes; a loading row shows its pending count.
- **Engine Map**: "Extract" becomes "Placement" (contents placed, slots owned); "LOD selection" loses the cap.
- **Trace**: `LevelsWanted`, `LevelsSwitched`, `LevelsUnloaded` stay; `ContentComposed` per content completed (rounds it took);
  `WorldExtracted` goes with the extract.
- Verbs and RPC: `lod-cap` and `{cap}` go; the rest stay.

## Verification

- **Composition unchanged**: `ngen-worldcheck` and `ngen-usdcheck` identical on `assets/layered/`, the test area and Caldera, with and without
  `--lod`.
- **Baselines**: the eight screenshots byte-identical.
- **Test area flight** (its authored `flight_NN` cameras, `wait-resident` at each): every root draws a level at every checkpoint; two runs give the
  same levels and counts; memory falls behind the camera; a tile switch takes under 0.25 s from `LevelsWanted` to `LevelsSwitched`.
- **Switch cost is proportional to the switch**: a tile switching levels changes only that tile's slots (the change list's instance count equals
  the two levels' draws), whatever else is loaded.
- **Simpler, measured**: the line count of `viewscene.cpp`, `worldcomposer.cpp` and `placement.*` (3,976 before step 4) is reported per step,
  not a bar (2026-10-16); why the two files grew is investigated separately, Caldera's share in particular.

## Open questions

- Should the composer still compose the whole stage per round, or only the contents a round touches? On the test world the stage is 16
  region prims; on Caldera it is not small.
- Prefetch (plan_streaming_lod decision 1's margin: load the next finer level within the band) fits decision 1 as an undrawn held level. Here,
  or with budgets?

## Deferred / follow-ups

- **Budgets, priorities, eviction under pressure** (plan_streaming_lod decision 4). Trigger: this landed.
- **Caldera revisited**: with the cap gone and levels composed whole, decide per problem whether the code or the authored structure (the
  converter in the fork) changes. Trigger: this landed.
- **Placements on the GPU** (decision 1 C). Trigger: meshlets, when the GPU expands placements into meshlet lists.
