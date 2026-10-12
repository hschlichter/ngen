# Streaming and LOD

**Status. In progress.** Revised 2026-10-12: the streaming structure is authored, not computed (decision 3); decisions 1, 2 and 4 stand, now in
terms of payloads; the questions that raised are decided (decisions 6 and 7). The steps it leads to are plans of their own.

A design doc, not an implementation plan: it decides how streaming and level of detail fit together, so the scene pack format
([plan_tool_architecture.md](plan_tool_architecture.md), step 3 of the roadmap's next steps) is built around the right payloads. Caldera is the
case it has to work for.

**Two different things.** *LOD* decides which representation of something to draw: per object, every frame, from a heuristic (screen size,
distance); it assumes the levels are there. *Streaming* decides what data is in memory: what to load, what to evict, in which order, within a
budget; it is about residency and I/O, not appearance. They meet because the levels LOD wants are what streaming most often has to deliver,
which is why decision 1 has one system give both outputs. Both are authored: a scene's LOD in its UsdLod roots, its streaming structure in its
payloads (decision 3). The engine reads them; it doesn't invent either.

**Resident** means loaded into memory and ready to draw. A payload's content (or a texture's mip level) goes through *requested* (the view has
asked for it), *packing* (the asset server is packing it), *packed* (in the server's cache), *fetching* (its data streaming to the view),
*resident* (received and uploaded into the GPU's geometry pool or textures) and *evicted* (dropped by the view; still packed). Today everything is
resident from start-up to exit; with streaming, only the part chosen to fit the budgets is.

## Current state

**The engine.** Everything loads at once and stays: the view opens the whole USD stage, builds every mesh and material
(`USDScene::updateAssetBindings`), extracts one `RenderWorld` and uploads it (`Renderer::uploadRenderWorld`). The geometry pool is one set of
buffers rebuilt wholesale on any geometry change (`GpuScene::rebuildGeometry`), the material textures sit in a fixed array of 1,024 slots
(`GpuScene::maxTextures`), and the RHI has no suballocator (one device allocation per resource). Culling is a flat GPU pass over every
instance (`src/renderer/README.md`); the editor's picking uses a CPU BVH (`SpatialIndex`). The asset server already streams on request, many ids
in one request (`src/asset/README.md`). The vendored OpenUSD is v26.08 and has the UsdLod schema; nothing in the engine uses it yet.

**The umbrella's scene pack design** ([plan_tool_architecture.md](plan_tool_architecture.md), "What ngen-view knows"): every layer packs into a
layer pack; a reference or payload target packs once into a sub-pack, instanced wherever it is used, and "payload boundaries are the load unit";
every variant of a variant set packs as alternative opinions, switched in the view without a repack. Packing is one job per file, each listing
the files it references, which become jobs of their own; nothing is computed over the whole scene.

**What USD has for streaming.** No streaming schema; a set of mechanisms streaming is built from:
- **payloads**, and which of them are loaded: `UsdStage::Load` and `Unload`, and **load rules** (`UsdStageLoadRules`: per path, all, only this
  prim, or none), USD's own way to say what is resident;
- **population masks** (`UsdStagePopulationMask`): compose only the prims under given paths;
- **`extentsHint`** (`UsdGeomModelAPI`): authored bounds of a model or payload, usable while it is unloaded;
- **model kinds** (`component`, `assembly`, `group`): the authored organisation into assets and groups of assets;
- **draw modes and proxies** (`model:drawMode`: bounds, cards, origin; `proxy` purpose with `proxyPrim`): stand-ins for what isn't drawn in full;
- **UsdLod**: which level to draw; each level can be its own payload;
- **instancing**: one prototype for every use of the same file.

**Caldera, measured** (`~/Code/caldera`; 63,700 binary `.usd` files, 9.7 GB; converted to UsdLod on a fork branch,
[plan_caldera_usdlod.md](plan_caldera_usdlod.md); throwaway probes against OpenUSD v26.08):

| | Prims | Meshes | Mesh points | Instances (prototypes) |
|---|---|---|---|---|
| Whole map, every district at its coarsest level (the default) | 53,640 | 19,044 | 14.6 M | 85 (57) |
| One district, `map_capital`, at full | 1,422,064 | 433,981 | 38.2 M (+ 9.5 M in prototypes) | 139,137 (2,815) |

| Payloads | Placements | Unique files behind them |
|---|---|---|
| The map as it opens | 836 | 138 |
| `map_capital` at full, finest level | 311,766 | 11,934 |
| The whole map at full, finest level (45 district and tile roots) | 2,854,341 | 40,702 |
| The whole map at full, every level | 3,430,258 | 43,733 |

- **Its streaming setup is authored, with standard USD.** Its README names the ways to manage the map's size: proxies, unloaded payloads, and
  loading sub-sections. Payloads mark what can load separately ("expensive leaf nodes … while leaving enough geometry to give a decent
  representation"); 159,303 of 159,393 payload prims in `map_capital` carry an `extentsHint`; kinds mark the hierarchy (72,433 `assembly`,
  473,176 `component`); the terrain is split into tiles (`st_*`). No draw modes are authored.
- **The work is per unique file**, 26 times fewer than placements: a file is loaded and packed once, however often it is placed.
- **The work is local**: 30,155 of the 40,702 files (74%) are used by one district or tile only; 10,547 kit pieces are shared. 25,804 files are
  placed once, 3,926 a hundred times or more (wall trims and baseboards, up to 3,074 placements).
- **Per district** (finest level): from 3 files (`map_exfil_ch3`) to 11,934 (`map_capital`); composing one in USD takes up to 14 s and 4.2 GB
  (`map_docks`, 382,227 placements), so the packer composes districts one at a time, never the whole island.
- **The scattered districts are small**: `mv_intel` (2.3 × 2.6 km across) has 7 payload placements; `map_gulags` (2.4 × 3.2 km) 52,591 placements
  of 555 files.
- **Its LOD** is UsdLod after conversion: 45 district and terrain-tile roots (`full`, `proxy`), 2,563 model roots with two to four levels, each
  level a payload. 5,510 single-level models are plain payloads.

**UsdLod** (`external/openusd/pxr/usd/usdLod/`): `LODRootAPI` on a prim makes its *child prims* the levels, ordered by index; `lod:heuristics`
targets heuristic prims (`LODDistanceHeuristic`, `LODScreenSizeHeuristic`, with thresholds and blend thresholds) per domain (imaging, physics,
audio); `LODOverrideAPI` overrides the choice for descendants; roots nest. Selection is left to the renderer. All levels are composed at once,
so streaming a level needs a payload on it.

## Scope

**In**
- Who decides what is resident: LOD selection, streaming, or one system for both.
- What loads and unloads (payloads, LOD levels, texture mip levels), and how each maps to USD.
- The streaming structure: what the scene's authors express, and how the engine reads it.
- Budgets, priority and eviction.
- What this requires of the scene pack format, the runtime scene library, the asset server and the renderer.

**Out**
- Implementation; the scene pack format itself (its own plan, which takes the requirements below).
- Occlusion culling, meshlets and LOD *generation* (simplifying meshes); Caldera ships its levels, and generation is a packer feature for later.
- Physics and audio LOD domains, beyond keeping the design open to them.
- Blending between levels (cross-fades, dithering); a level switches outright for now.

## Decisions

### 1. Who decides what is resident

- **A. LOD selection decides.** Each frame the LOD heuristic picks a level per LOD root; streaming fetches what was picked. Simple, one source
  of truth, but nothing is fetched before it is needed: every switch shows a hole or a stale level while it loads.
- **B. A separate streaming system.** Its own priorities (distance, budget) decide what is resident; LOD picks among what is resident. Allows
  prefetching, but two systems compute nearly the same distances and can disagree: LOD wants a level streaming hasn't loaded, streaming keeps
  a level LOD never shows.
- **C. One system, two outputs.** Per LOD root the heuristic gives a *wanted* level; residency is the wanted level plus a prefetch margin (the
  next finer level when within a band of switching), and the coarsest level of every LOD root within resident content is always resident (the
  floor). The budget trims from the lowest priority. Drawing shows the finest resident level that is not finer than the wanted one, so a
  missing level falls back to a coarser one, never to a hole.

**Locked: C** (2026-10-11). One computation, prefetching without a second system, and the coarse level doubling as the placeholder is the
"everything async" behaviour the roadmap asks for. Its cost is the floor; Caldera's is the proxy map, 14.6 M points. A level whose content is itself
payloads is drawn as soon as its own payload is resident (decision 6).

### 2. What loads

**UsdLod is the only LOD the engine reads.** Locked (2026-10-11). A LOD root's children are the levels, finest first, each a separately
loadable payload with its own bounds; its heuristics are the selection rule. Data that expresses LOD any other way is converted to UsdLod
before it is packed, outside the engine: Caldera's `districtLod` and `modelLod` variant sets were rewritten into UsdLod roots by a converter in
a Caldera fork ([plan_caldera_usdlod.md](plan_caldera_usdlod.md)).

**What loads.** Locked (2026-10-12).
- **Geometry: a payload's content.** A payload target is one file; it packs once into a sub-pack (one pack job per file), and every placement
  of it is an instance of that sub-pack. What is loaded and evicted is the sub-pack: loaded once, kept while any resident placement uses it.
  A placement itself is an entry in its parent's pack (which sub-pack, where, its bounds) and exists whether or not its sub-pack is resident.
- **A LOD level** is a payload like any other: switching level loads the finer level's sub-pack and, once drawn, lets the coarser one go
  (except the floor).
- **Textures: mip levels.** A texture packs as a *mip tail* (the levels up to a small size, say 128², always loaded with its first user) and the
  larger levels as parts of their own, lowest first. The wanted level comes from the screen size of the instances using it, estimated on the
  CPU from bounds; GPU feedback is deferred. Caldera has no textures; Sponza is the test.

### 3. The streaming structure is authored

**Locked** (2026-10-12): the scene's authors define its streaming structure with standard USD, and the packer translates it; nothing is
computed over the scene at pack time, and no new schema is introduced.
- **Payloads are what streams.** What sits behind a payload streams as one; the author decides where payloads go, as Caldera's authors did.
  Nothing groups payloads further (no assemblies or tiles as streaming groups): decided 2026-10-12, revisited only if a scene needs it.
- **Bounds are `extentsHint`**, authored on the payload prim (or the model above it), so a placement can be judged near or far, visible or not,
  while its content is unloaded.
- **Residency is load rules**: what the view has resident can be written as a `UsdStageLoadRules` (all, only, none per path), the same
  vocabulary the editor and USD tools use.

A scene authored as one large file is one payload's worth of content and one pack job: that is the author's choice, and changing it is an authoring change, not
something the engine works around.

### 4. Budgets, priority, eviction

Locked (2026-10-11).
- **Budgets**: GPU geometry, GPU textures, CPU resident packs, the view's cache of fetched packs. Each is a number in the project settings, read
  as a record.
- **Priority is per placement**: the screen-space error of not having a payload's content there (its bounds over the distance, the level's
  detail step), raised for placements drawn last frame (the culling readback already says which instances were drawn), lowered for placements
  behind the camera. A file's priority is its highest placement's.
- **Eviction is per file**: a payload's sub-pack is evicted when no placement wants it, lowest priority first, never the floor; hysteresis (a
  payload loaded stays at least N seconds or until it is far out of its band) so the camera moving back and forth doesn't thrash.

### 5. What this requires of the scene pack format and the engine

Carried into the scene pack plan ([plan_scene_pack.md](plan_scene_pack.md): placements, LOD roots and the level per instance in its decisions 3,
17 and 18; the renderer and asset server parts are left to the streaming steps).
- **Scene pack**: per placement of a payload, which sub-pack, its transform and its bounds (from `extentsHint`, or none: decision 6); LOD roots
  (levels, bounds, heuristics), nesting (a converted Caldera district's levels hold model roots of their own) and sitting inside instance
  prototypes (a model asset is instanceable: a root is per prototype, its level chosen per instance); textures as mip tail plus level parts. One
  sub-pack per payload target file, from one pack job.
- **Asset server**: requests for pack *parts* (a texture's mip level), and the request flow and priorities of decision 7.
- **Runtime scene library**: placements exist with their bounds whether or not their content is resident; sub-packs come and go, counted by the
  resident placements using them.
- **Renderer**: the geometry pool takes ranges in and out without a rebuild (a suballocator in the pool, not in the RHI yet); the instance
  buffer takes instances in and out; texture slots are allocated and freed, and a texture's resident levels change under a fixed slot
  (`minLod` clamped to what is resident); the culling pass reads which level of each LOD root to draw.

### 6. Draw what is resident, as soon as it is

Locked (2026-10-12). Nothing waits for anything else: a LOD root draws its finest resident level not finer than the wanted one (decision 1),
and content under it appears payload by payload as it becomes resident, never held back for siblings, children or anything still packing.
Districts are Caldera's concept, not USD's or ngen's: to the engine a converted district is a LOD root and its levels are payloads like any
other.

Accepted consequence: when a LOD root switches to a finer level, the coarser level stops drawing once the finer level's own payload is
resident, and the payloads under it arrive one by one. A district switching to `full` stops drawing its proxy when its prefab is in; its
models then appear as they arrive, with gaps where they haven't yet.

A payload without `extentsHint` has no bounds to judge, so it is always wanted while its parent is resident, and the packer reports it as a
trace warning naming the prim. Adding the bounds is an authoring fix; the engine doesn't measure or guess them (90 of 159,393 payload prims in
`map_capital`). This applies to payloads that aren't LOD levels: a level is wanted through its root (decision 1), and its bounds are the root's
heuristic extent, so a level prim needs none of its own (the converted Caldera's levels have none).

### 7. Requests and priorities

Locked (2026-10-12). Every payload the view wants is requested, however many: the asset server queues them (12,000 for flying into
`map_capital` is fine). Each request carries the payload's priority (decision 4), nearest first, and the view can change a request's priority
as the camera moves. Nothing is cancelled: a request the view no longer needs still completes on the server, so the packed result is cached
for next time, and the view ignores the answer. The server does two jobs, packing and delivering: what is already packed is delivered at once,
in priority order, while packing works through its queue; a full queue makes new packing slower, not delivery.

A request moves through *requested*, *packing*, *packed*, *fetching* and *resident* (the definitions above). Whether the view fetches a packed
payload's data only once it still wants it (a separate step after *packed*, so data it no longer needs isn't sent) or the server streams it
right after packing, as today, is for the implementation plan.

### Replaced

- **A uniform XY grid built by the packer, with one cell pack per LOD level of a cell** (locked 2026-10-11, replaced 2026-10-12). The grid was
  layout computed at pack time, not authored, so a designer couldn't control or predict it; and its motivation, one request per payload
  placement (311,765 in one district), measured as one load per unique file (11,934). Rejected alongside it then: the authored hierarchy alone,
  and a quadtree.
- **Per-cell coarse levels cut by the packer** (deferred 2026-10-11, dropped 2026-10-12): splitting a district's proxy by cell is the packer
  inventing structure. If a district's coarse level is too coarse, that is an authoring change.
- **Cancelling requests the view no longer needs** (proposed 2026-10-11, replaced 2026-10-12 by decision 7): the server finishes them so the
  result is cached; the view ignores the answer.

## Steps

1. **Lock the decisions above** (this doc; 2026-10-11, revised 2026-10-12), then move the requirements of decision 5 into the scene pack plan
   when it is written.
2. **Caldera's proxy map through the scene pack**: the packer packs the converted `caldera.usda` ([plan_caldera_usdlod.md](plan_caldera_usdlod.md))
   with every root's coarsest level; the view loads it. No streaming yet, but the placements with their bounds, the sub-packs and the LOD roots
   exist and are inspected as records.
3. **Districts stream**: the district roots' levels load and unload by their heuristics as the camera moves.
4. **Models stream**: the model roots inside loaded districts, by screen size.
5. **Textures stream** on Sponza: mip tails at load, larger levels by screen size, within the texture budget.

Each of steps 2–5 is its own plan, written when the one before has landed.

## Verification

For this doc: the decisions are locked or listed under Open questions, and decision 5 is copied into the scene pack plan. For the steps it
leads to, the observable criteria to carry into their plans:

- Caldera opens to a first frame within the proxy map's budget (the proxy stage opens in 142 ms today; the packed map should not be slower),
  and the `memory` record stays under the configured budgets during a scripted fly-over.
- A scripted camera path over Caldera gives the same image at the same frame on two runs, and no frame shows a hole: every visible LOD root
  draws at least its floor.
- What is resident at a frame can be written as `UsdStageLoadRules`; loading the converted stage in USD with those rules composes the same
  placements the view has resident.
- Loads and evictions show in the trace as flow per batch (payloads loaded, evicted, with counts and time), not per placement, and the
  `streaming` record holds what is resident, wanted and in flight, with each request's state and priority.

## Open questions

- Caldera's heuristic thresholds: the converter generates a starting point; the engine tunes them.
- What the floor costs at Caldera scale: the proxy map is 14.6 M points; is it the floor everywhere, or does the far field need a coarser one?

## Deferred / follow-ups

- **GPU feedback for texture levels** (which levels the sampler touched). Trigger: CPU estimates loading levels never seen, or missing ones.
- **Level blending** (cross-fade or dither between levels). Trigger: visible popping at level switches.
- **LOD generation in the packer** (mesh simplification for assets without authored levels). Trigger: a scene without levels that is too
  heavy to draw at full detail.
- **Physics and audio LOD domains.** Trigger: physics (roadmap, Engine features).
- **Draw modes** (`model:drawMode` bounds or cards) as the stand-in for unloaded content. Trigger: a scene that authors them.
