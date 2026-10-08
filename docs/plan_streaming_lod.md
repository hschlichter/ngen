# Streaming and LOD

**Status. Draft.**

A design doc, not an implementation plan: it decides how streaming and level of detail fit together, so the scene pack format
([plan_tool_architecture.md](plan_tool_architecture.md), step 3 of the roadmap's next steps) is built around the right load units. Caldera is the
case it has to work for. Decisions here are recommendations until locked; pushback welcome on each.

**Two different things.** *LOD* decides which representation of something to draw: per object, every frame, from a heuristic (screen size,
distance); it assumes the levels are there. *Streaming* decides what data is in memory: what to load, what to evict, in which order, within a
budget; it is about residency and I/O, not appearance. They meet because the levels LOD wants are what streaming most often has to deliver,
which is why decision 1 has one system give both outputs; but a scene's LOD (its UsdLod roots, authored or converted) says nothing about
streaming, and the engine's streaming units (cells, cell packs) are the packer's, not the source data's.

## Current state

**The engine.** Everything loads at once and stays: the view opens the whole USD stage, builds every mesh and material
(`USDScene::updateAssetBindings`), extracts one `RenderWorld` and uploads it (`Renderer::uploadRenderWorld`). The geometry pool is one set of
buffers rebuilt wholesale on any geometry change (`GpuScene::rebuildGeometry`), the material textures sit in a fixed array of 1,024 slots
(`GpuScene::maxTextures`), and the RHI has no suballocator (one device allocation per resource). Culling is a flat GPU pass over every
instance (`src/renderer/README.md`); the editor's picking uses a CPU BVH (`SpatialIndex`). The asset server already streams on request, one
asset at a time or in batches (`src/asset/README.md`). The vendored OpenUSD is v26.08 and has the UsdLod schema; nothing uses it yet.

**The umbrella's scene pack design** ([plan_tool_architecture.md](plan_tool_architecture.md), "What ngen-view knows"): every layer packs into a
layer pack; a reference or payload target packs once into a sub-pack, instanced wherever it is used, and "payload boundaries are the load unit";
every variant of a variant set packs as alternative opinions, switched in the view without a repack; nested variant sets are packed with their
default selection only (an open question there).

**Caldera, measured** (`~/Code/caldera`, opened with OpenUSD v26.08 by a throwaway probe; 63,700 binary `.usd` files, 9.7 GB):

| | Prims | Meshes | Mesh points | Payloads | Instanceable | `modelLod` sets |
|---|---|---|---|---|---|---|
| Whole map, every district at `proxy` (the default) | 53,640 | 19,044 | 14.6 M | 836 | 85 | 418 |
| One district, `map_capital`, at `full` | 1,422,064 | 433,981 | 38.2 M (+ 9.5 M in 2,868 prototypes) | 311,765 | 139,137 | 152,835 |

- The proxy map opens in 142 ms. Switching one district to `full` takes 3 s of composition alone.
- **Two authored LOD levels, both variant sets.** `districtLod` (`proxy`, `full`) on 36 districts and 14 terrain tiles (`st_*`); `modelLod`
  (`lod0` … `lod3`) on models, nested inside the `full` variant. Not every model has every level: `[lod0]` 85,964, `[lod0 lod1]` 32,243,
  `[lod0 lod1 lod2 lod3]` 9,945, `[lod0 lod3]` 4,391, and other gaps. Every model selects `lod0`, the finest.
- **Payloads are fine-grained.** 311,765 in one district, on `Xform` and `SkelRoot` models: one per model, not one per area.
- **Districts are not all compact.** Most are 0.3–1 km across, but `map_gulags` (2.4 × 3.2 km) and `mv_intel` (2.3 × 2.6 km) are collections
  spread over most of the island, and `map_vista`, the backdrop, is 26 km across. The authored hierarchy is semantic, not spatial.
- **Instancing matters.** 139,000 instances of 2,868 prototypes in one district.
- Guide-purpose prims (volumes, collision, annotations) are 12% of the prims; no textures or materials.

**UsdLod** (`external/openusd/pxr/usd/usdLod/`): `UsdLodRootAPI` on a prim makes its *child prims* the LOD items, ordered by index;
`lod:heuristics` targets heuristic prims (`DistanceHeuristic`, `ScreenSizeHeuristic`, with thresholds and blend thresholds) per domain
(imaging, physics, audio); `UsdLodOverrideAPI` overrides the choice for descendants; roots nest. Selection is left entirely to the renderer.
The levels are children, so all of them are composed at once, unlike a variant set, which composes one; streaming a UsdLod level needs a
payload on that child.

## Scope

**In**
- Who decides what is resident: LOD selection, streaming, or one system for both.
- The units: what loads and unloads (geometry and sub-packs, LOD levels, texture mip levels), and how each maps to USD (payloads, UsdLod
  roots).
- The spatial layout: the cells the packer builds, shared by streaming, coarse culling and LOD.
- Budgets, priority and eviction.
- What this requires of the scene pack format, the runtime scene library, the asset server and the renderer.

**Out**
- Implementation; the scene pack format itself (its own plan, which takes the requirements below).
- Occlusion culling, meshlets and LOD *generation* (simplifying meshes); Caldera ships its levels, and generation is a packer feature for later.
- Physics and audio LOD domains, beyond keeping the design open to them.
- Blending between levels (cross-fades, dithering); a level switches outright for now.

## Decisions

### 1. Who decides what is resident

- **A. LOD selection decides.** Each frame the LOD heuristic picks a level per LOD group; streaming fetches what was picked. Simple, one
  source of truth, but nothing is fetched before it is needed: every switch shows a hole or a stale level while it loads.
- **B. A separate streaming system.** Its own priorities (distance, budget) decide what is resident; LOD picks among what is resident. Allows
  prefetching, but two systems compute nearly the same distances and can disagree: LOD wants a level streaming hasn't loaded, streaming keeps
  a level LOD never shows.
- **C. One system, two outputs.** Per LOD group the heuristic gives a *wanted* level; residency is the wanted level plus a prefetch margin
  (the next finer level when within a distance band of switching), and the coarsest level of every group in a loaded cell is always resident
  (the floor). The budget trims from the lowest priority. Drawing shows the finest resident level that is not finer than the wanted one, so a
  missing level falls back to a coarser one, never to a hole.

I lean **C**: one computation, prefetching without a second system, and the coarse level doubling as the placeholder is exactly the "everything
async" behaviour the roadmap asks for. Its cost is that the floor must be cheap enough to keep for every loaded cell; Caldera's proxies are.

### 2. The units

**UsdLod is the only LOD the engine reads.** Locked. A LOD group is a UsdLod root: its children are the levels, finest first, each a
separately loadable unit with its own bounds; its heuristics are the selection rule. Data that expresses LOD any other way is converted to
UsdLod before it is packed, outside the engine: Caldera's `districtLod` and `modelLod` variant sets are rewritten into UsdLod roots by a
converter in a Caldera fork ([plan_caldera_usdlod.md](plan_caldera_usdlod.md)). Nothing in the packer or the view knows a project's LOD
conventions.

**What loads.**
- **Geometry: the sub-pack**, as the umbrella says, but not one request per payload. 311,765 payloads in one district would be 311,765
  requests; the packer groups the sub-packs of a cell and level into one *cell pack*, the unit requested and evicted, while prototypes shared
  across cells stay sub-packs of their own (packed once, loaded on first use, kept while any cell uses them).
- **A LOD level of a group**: the level's cell pack entries. Switching level loads the finer level's units and, once drawn, lets the coarser
  ones go (except the floor).
- **Textures: mip levels.** A texture packs as a *mip tail* (the levels up to a small size, say 128², always loaded with its first user) and the
  larger levels as units of their own, lowest first. The wanted level comes from the screen size of the instances using it, estimated on the
  CPU from bounds; GPU feedback (which levels the sampler actually touched) is deferred. Caldera has no textures; Sponza is the test.

I lean **cell packs over sub-packs per payload**. Open: whether a cell pack holds one level of one
cell, or all levels of a cell with levels addressable inside it (fewer files, partial reads).

### 3. The spatial layout

- **A. The authored hierarchy** (districts, tiles, LOD roots). Free and meaningful, but not spatial: `map_gulags` and `mv_intel` cover most of
  the island, so "load what is near" would load them whole.
- **B. A uniform 2D grid**, built by the packer over the bounds of every placed sub-pack. Caldera is an island, 2 × 2 miles in X and Y and
  shallow in Z: a grid in XY with a cell size around 100–200 m gives a few hundred cells. Simple to build, to query and to stream by distance.
- **C. A quadtree.** Adapts cell size to density (the town centre finer than the sea), at the cost of variable cell sizes for streaming and
  culling.

I lean **B, a uniform XY grid, with the authored hierarchy kept as the LOD structure inside it**: cells say *where*, LOD groups say *how
detailed*. A group belongs to the cell its bounds' centre falls in; groups much larger than a cell (`map_vista`, terrain tiles) belong to a
coarse level of the grid (cells of 4 × 4 cells), so the grid is two or three levels deep rather than a full quadtree. The same cells are the
coarse culling groups (two-level culling) and the streaming units. Per-frame culling stays flat on the GPU; the editor's BVH stays as it is.

### 4. Budgets, priority, eviction

- **Budgets**: GPU geometry, GPU textures, CPU resident packs, the view's cache of fetched packs. Each is a number in the project settings, read
  as a record.
- **Priority** of a unit: the screen-space error of not having it (the group's bounds over the distance, the level's detail step), raised for
  cells visible last frame (the culling readback already says which instances were drawn), lowered for cells behind the camera.
- **Eviction**: lowest priority first, never the floor of a loaded cell; hysteresis (a unit loaded must stay at least N seconds or until it is
  far out of its band) so the camera moving back and forth doesn't thrash.

### 5. What this requires of the scene pack format and the engine

- **Scene pack**: the grid (cell size, levels, each cell's bounds and units), LOD groups (one per UsdLod root: its levels, bounds and
  heuristics), cell packs per cell and level, prototype sub-packs, and textures as mip tail plus level units. LOD roots nest (a converted
  Caldera district's levels hold models that are LOD roots of their own) and sit inside instance prototypes (a model asset is instanceable):
  a group is per prototype, its level chosen per instance. With LOD as UsdLod roots rather than variant sets, the umbrella's open question
  on nested variant sets doesn't block LOD.
- **Asset server**: requests for pack *parts* (a cell pack, a mip level) and priority with cancellation (both on the roadmap, "Assets and
  packing"); a request that is no longer wanted is cancelled, not finished.
- **Runtime scene library**: units come and go; an entity exists in the scene whether or not its geometry is resident.
- **Renderer**: the geometry pool takes ranges in and out without a rebuild (a suballocator in the pool, not in the RHI yet); the instance
  buffer takes instances in and out; texture slots are allocated and freed, and a texture's resident levels change under a fixed slot
  (`minLod` clamped to what is resident); the culling pass reads which level of each group to draw.

## Steps

1. **Lock the decisions above** (this doc), then move the requirements of decision 5 into the scene pack plan when it is written.
2. **Caldera's proxy map through the scene pack**: the packer packs the converted `caldera.usda` ([plan_caldera_usdlod.md](plan_caldera_usdlod.md))
   with every root's coarsest level; the view loads it as cells.
   No streaming yet, but the grid, LOD groups and cell packs exist and are inspected as records.
3. **Districts stream**: the district roots as LOD groups; moving the camera loads the full level for districts near it and returns to the proxy
   level when it leaves.
4. **Models stream**: the model roots inside loaded districts, by screen size.
5. **Textures stream** on Sponza: mip tails at load, larger levels by screen size, within the texture budget.

Each of steps 2–5 is its own plan, written when the one before has landed.

## Verification

For this doc: the decisions are locked or listed under Open questions, and decision 5 is copied into the scene pack plan. For the steps it
leads to, the observable criteria to carry into their plans:

- Caldera opens to a first frame within the proxy map's budget (the proxy stage opens in 142 ms today; the packed map should not be slower),
  and the `memory` record stays under the configured budgets during a scripted fly-over.
- A scripted camera path over Caldera gives the same image at the same frame on two runs, and no frame shows a hole: every visible group draws
  at least its floor (a `capture` of the frame's draw list has a level for every group in view).
- Loads and evictions show in the trace as flow per cell (cell loaded, cell evicted, with counts and time), not per sub-pack, and the
  `streaming` record holds what is resident, wanted and in flight.

## Open questions

- Cell pack granularity: one level of one cell per pack, or all levels of a cell in one pack with addressable parts.
- Caldera's heuristic thresholds: the converter generates a starting point ([plan_caldera_usdlod.md](plan_caldera_usdlod.md)); the engine
  tunes them.
- Cell size, and whether two or three grid levels are enough for Caldera's large groups.
- What the floor costs at Caldera scale: the proxy map is 14.6 M points; is it the floor everywhere, or does the far field need a coarser one?
- **Area-scale LOD (HLOD) against cells.** A converted Caldera district is a UsdLod root whose coarse level is one proxy mesh for the whole
  district and whose fine level is everything in it. Per-model LOD fits streaming by cells without trouble; district LOD does not: a
  district's single heuristic over its whole extent (`map_gulags` and `mv_intel` spread over most of the island) makes all of it want `full`
  when the camera is near any part, and one proxy mesh can't be shown for the far half of a district while the near half is in full. Two
  answers:
  1. **District LOD as authored**: a district switches as a whole; its full level streams by cells underneath it. Simple; the proxy-or-full
     decision is coarse.
  2. **The packer cuts area-scale levels per cell**: it splits the proxy mesh by cell, so the coarse level is per cell and lines up with
     streaming. Behaves at Caldera's scale; a packer feature.

  I lean 1 first, 2 when Caldera shows the coarse switch (a district's whole full level loading for a corner of it). This is a packer and
  engine question; the converted data stays as it is either way.

## Deferred / follow-ups

- **GPU feedback for texture levels** (which levels the sampler touched). Trigger: CPU estimates loading levels never seen, or missing ones.
- **Level blending** (cross-fade or dither between levels). Trigger: visible popping at level switches.
- **LOD generation in the packer** (mesh simplification for assets without authored levels). Trigger: a scene without levels that is too
  heavy to draw at full detail.
- **Physics and audio LOD domains.** Trigger: physics (roadmap, Engine features).
- **A quadtree instead of the grid.** Trigger: one cell size can't fit both dense and empty areas within budget.
