# Variant sets and PointInstancer

**Status. In progress.** Decisions 1 to 6 locked 2026-10-16, each as recommended; steps 1, 2 and 2b done 2026-10-16. Lands before
[plan_streaming_v2.md](plan_streaming_v2.md) step 4 (placements), so the placement tree is built with both.

## Current state

The streaming test world (`assets/streamtest-world/world/`, [test_streaming_world.md](test_streaming_world.md)) is best-practice USD, and two
things in it the engine doesn't read:

- **Variant sets.** Every placed asset selects a `shape` variant (trees `a`/`b`/`c`, about 11,200 placements in one tile; rocks `flat`/
  `angular`/`round`), and the asset's geometry (a reference to `*_geom.usdc`) and its LOD heuristics live inside the variant. The packer
  warns "variants aren't read" per placement and packs nothing from a variant (src/asset/README.md, "Scene packs"); the composer composes no
  variant arc (src/world/README.md, "Not composed"). So every asset is empty: with a tile at `full`, 91,516 of `ngen-worldcheck`'s 140,789
  lines differ from `ngen-usdcheck`'s, all under `Geom/lod*`, and USD has 25 prototypes to the engine's 9.
- **`UsdGeomPointInstancer`.** Ground cover is point-instanced: chunks of about 17,000 points (`positions`, `orientations`, `scales`,
  `ids`, `protoIndices`), prototypes under the instancer that reference scatter assets (no variants or LOD inside). The engine composes the
  prototypes as ordinary prims (and draws them once, where they sit) and nothing per point.

Also found on the test world: an asset's LOD levels are `over`s outside the variant, defined only through it. UsdLod counts only defined
children as LOD items; the engine counts every child.

## Scope

In:
- Variant sets: packing variant specs and selections, composing variant arcs as Pcp does (strength order, selection from the whole prim
  index, nested sets), instancing keyed by them.
- `PointInstancer`: packing its arrays and prototypes, composing it into a World component, drawing every point's prototype.
- LOD items only from defined children.
- `ngen-usdcheck` / `ngen-worldcheck` parity for both, and `assets/layered/` cases for both.

Out:
- Variant fallbacks (USD's plugInfo `variantFallbacks`): no scene uses them; a set with no selection composes no variant, as USD does
  without fallbacks.
- Authoring or switching variant selections at runtime (the editor's).
- PointInstancer motion (`velocities`, `accelerations`, time samples), `masks` beyond `invisibleIds`.
- Per-point LOD inside point-instanced prototypes (decision 4).
- Drawing on the GPU from the instancer's arrays (with meshlets; Deferred).

## Decisions

### 1. Variants in the layer pack

The packer reads one file, as Sdf (no composition). A variant is opinions in that file under `/prim{set=name}`.

- **A. Pack variant specs as prim specs at their variant paths** (`/juniper{shape=a}`, `/juniper{shape=a}Geom`), with each prim's
  `variantSetNames` and its `variants` selections as fields. The composer sees them as more specs of the same layer.
- **B. Pack each variant as a nested section under its prim.**

**Locked: A** (2026-10-16): the composer already finds specs by path in a layer; a variant node is a site with a variant path. Format version bump.

### 2. Composing variant arcs

As `pcp` does it: a prim's variant sets are the ones authored at any node of its index; each set's selection is the strongest `variants`
opinion for it across the whole index (a referencing layer's selection beats the asset's own), and with none, no variant arc. The variant
node is a child of the node whose layer stack authors the set, at `<path>{set=selection}`, ordered among that node's arcs after inherits and
before references and payloads (LIVRPS). A variant's own variant sets compose the same way, nested. Instances: the variant nodes are part of
the kept nodes, so different selections are different prototypes, as in USD.

**Locked** (2026-10-16): there is no real alternative here; anything else is not USD's composition, and `ngen-usdcheck` parity is the bar.

### 3. PointInstancer in the World

- **A. Expand at compose**: an entity per point. Points are tens of thousands per chunk; the World would hold millions of entities the
  scene doesn't author.
- **B. A component**: `PointInstancerData` per instancer entity: its prototype entities (from the `prototypes` relationship, in this
  content) and per point its prototype index, local transform (`positions`, `orientations`, `scales`, as `UsdGeomPointInstancer` composes
  them) and visibility (`invisibleIds`). The prototypes stay entities under the instancer, not drawn where they sit (as Hydra treats prims
  under a PointInstancer's prototypes).

**Locked: B** (2026-10-16): the World holds what the scene authors; points are data. It is also v2's placement: one placement per instancer, its slots
per point.

### 4. LOD inside point-instanced prototypes

A prototype with a LOD root would pick its level per point.

- **A. Per point**: correct, a root per point (tens of thousands per chunk) evaluated by the heuristic.
- **B. The prototype's default level for every point**, with a trace message naming the instancer.

**Locked: B** (2026-10-16) for this plan: the test world's prototypes have no LOD inside, and per-point selection belongs with the GPU-side placement
(Deferred). The instancer itself sits inside a tile level, so it streams with the tile.

### 5. Drawing points before v2's placements

v2 step 4 replaces the extract with placements. Points need drawing now to verify this plan, and again in step 4.

- **A. Draw them in today's extract**: a prototype's walk kept once, replayed per point (as an instance's prototype is today). Rewritten
  in step 4, but small: the replay already takes a transform per use.
- **B. No drawing here**: the plan composes and checks parity; points first draw in v2 step 4.

**Locked: A** (2026-10-16): the test world's ground cover is what makes it look like a world, and a draw count per tile is the measure step 4 must keep.

### 6. Checking points against USD

**Locked** (2026-10-16). `ngen-usdcheck --instancers` prints per instancer its path, point count and every point's world matrix
(`UsdGeomPointInstancer::ComputeInstanceTransformsAtTime`), sorted by id; `ngen-worldcheck --instancers` prints the same from the World.
Compared as files, as the fingerprints are.

## Steps

1. **Packer: variants** (decision 1): `src/asset/pack/usd/usdpacker.cpp` packs variant prim specs at their variant paths, and per prim
   `variantSetNames` and `variants`; the "variants aren't read" warning goes. `pack/scenepack.h` gains the two fields; format version and
   the `usd` rule version bumped.
2. **Composer: variant arcs** (decision 2): `src/world/worldcomposer.cpp` adds variant nodes when building a prim index, with selections
   from the whole index and LIVRPS order; prototype keys include them. `assets/layered/` gets an asset with a variant set selected by the
   layer that references it, and a nested set.
2b. **Material interface inputs** (added 2026-10-16, option B of three: packing materials from the composed stage, composing their inputs,
   or leaving the terrain white): a material that references another file's network and sets its inputs (the test world's terrain: the
   tiles reference `library.usda`'s `terrain` and set `inputs:color_texture`) is composed, not packed: the material pack names the input
   a texture's file comes from, the layer pack keeps a material's own `inputs:*`, the composer composes them on the material entity and a
   binding names the material as a key (pack and input values), and the view applies the values to the pack's textures. Packing stays per
   file; composition stays in the composer. Also: a `Material` whose file doesn't author its network packs no material (it used to pack an
   empty one, which outranked the referenced library's and drew white).
3. **LOD items from defined children**: a LOD root's levels are its children that are defined (and active, not abstract), as UsdLod counts
   them.
4. **Packer and World: PointInstancer** (decision 3): the packer keeps `positions`, `orientations`, `scales`, `protoIndices`, `ids`,
   `invisibleIds` and the `prototypes` relationship (half and float quaternions to float); `World` gains `PointInstancerData` per
   content; `World::visit` skips prims under an instancer's prototypes. `assets/layered/` gets a small instancer.
5. **Parity** (decision 6): `--instancers` in both tools; `src/world/fingerprint.h` prints the lines.
6. **Drawing points** (decision 5, A): the extract walks each prototype once and replays it per visible point; draws carry the
   instancer's prim for picking. Prototype LOD roots at their default (decision 4).
7. **Visibility pass and docs**: `src/world/README.md` (variants and instancers composed; "Not composed" shrinks), `src/asset/README.md`
   (what a layer pack holds), `src/view/README.md`.

### Step 1 results (2026-10-16)

- The layer pack keeps each prim's `variantSetNames` and `variants`, and every spec inside a variant at its variant path, nested variants
  included (scene pack format 3, usd rule version 5); the two variant warnings are gone. `ngen-packdump` prints both fields.
- A mesh or material defined inside a variant packs from the file-local stage with that variant selected (a session layer selecting it on
  the prims owning the sets), its asset id its variant path. Checked on a file with a nested set: the cube in `shape=a` and the sphere in
  `shape=b` pack as themselves, the sphere with the display colour its nested `color=red` variant authors. The test world defines nothing
  inside its variants (its variants hold overs and references), so this is for scenes that do.
- Until step 2 composes them, the composer finds variant specs by path but doesn't list them as children (their string parent would
  otherwise make `/asset{shape=a}Geom` a root prim): composition is exactly as before, the test world's tile at `full` still differing in
  the same 91,516 lines.
- The test world packs with no warnings; `assets/layered/` with its two known ones. Baselines byte-identical; `ngen-worldcheck` against
  `ngen-usdcheck` unchanged on the layered scene, `three_cubes` and the test world.
- Seen, not changed: "an over authors geometry" warns on an over in the same file as the definition, which the file-local stage does apply
  (the nested `color=red` over above). An old imprecision of the rule, more visible with variants.

### Step 2 results (2026-10-16)

- **Variant arcs** in `WorldComposer`: the sets authored on any node, each selection the strongest in the whole index, the variant node under
  its set's author before references (`ArcKind::Variant`), its arcs added with it and the index scanned again until no variant is added. An
  arc's target leaves its own sets to the prim composed (`indexAt(..., resolveVariants = false)`); its ancestors resolve theirs. The path
  helpers read variant paths (`/asset{shape=a}Geom`'s parent is `/asset{shape=a}`), and an arc authored inside a variant maps to the path
  without selections. Step 1's guard became that.
- **`assets/layered/variants.usda`** and its placements: the asset's own selections, the referencing layer's (which win), a nested set
  selected from outside, geometry defined inside a variant, references inside a variant (to `model.usda`, `building.usda`), instances
  splitting into prototypes by selection, `override.usda`'s selection beating `base.usda`'s, and one naming a variant that isn't there (a
  `CompositionMessage`). `ngen-worldcheck` against `ngen-usdcheck` identical on all of it (only `/World/Inherited` differs, as before).
- **The test world**: with a tile at `full`, 183,301 lines and none differing (91,516 before), 25 prototypes in both. At `dense_forest`
  60,150 draws (209 before): trees, bushes and logs draw; peak resident memory 829 MB.
- Caldera at its default levels identical as before (54,124 prims, 57 prototypes); baselines byte-identical.
- **Seen, not fixed**:
  - A mesh's own attributes come from its mesh pack, packed once from the file that defines it with that file's selections: an `over` in a
    variant selected elsewhere (the nested `color = "blue"` above) doesn't reach it, so `Selected`'s sphere draws red where USD has it blue.
    The fingerprint doesn't compare display colours. Primvars set per variant need the mesh's attributes read where composed, not packed
    once; a plan of its own if the test world relies on it (it doesn't author display colours in variants).
  - The test world's assets draw white, foliage cards opaque: their materials (bound inside the asset, referencing
    `materials/library.usda`) aren't read. The terrain's white is likely the same. Next to look into.

### Step 2b results (2026-10-16)

- **No network, no material**: `packMaterial` gives no material for a `Material` whose file-local stage has no connected surface; the
  assets' materials (`def Material` referencing `materials/library.usda`) now draw the library's: bark textures and alpha-tested foliage
  at `dense_forest` (they drew white, foliage opaque). usd rule version 6.
- **Interface inputs**: `MaterialPack::baseColorTextureInput` / `opacityTextureInput` (scene pack format 4, usd rule version 7); the layer
  pack keeps a material's `inputs:*`; `Entity::materialInputs` composed strongest-first; `materialKey` / `parseMaterialKey` in `world.h`;
  `ViewScene::resolveMaterial`. The test world's terrain draws its tiles' `regions/R_2_2/terrain_color.jpg` and, as the region proxy,
  `terrain_color_far.jpg` (the library's own default, `terrain_grass.jpg`, isn't used); the world reads as a world at `four_tiles`.
- **Test case**: `assets/layered/looks.usda`, a library material whose texture file is its input with no default; `base.usda`'s
  `TexturedUse` references it and sets `inputs:tex`, bound to `InterfaceCube`. In a scene of that cube alone the checker texture is
  requested (the only name for it is the input). Generated shapes (`Cube`) have no texture coordinates, so the cube shows one texel.
- Baselines byte-identical; `ngen-worldcheck` against `ngen-usdcheck` unchanged (the layered scene, the test world with a tile at `full`).
- Not covered: constants set through an input (a `diffuseColor` connected to the interface), and interface inputs on node graphs. The test
  world uses neither.

## Visibility

- **`instancers` record** (new): per instancer reached, its path, points, visible points, prototypes, and draws.
- **`scene` record**: instancers and points reached, beside prims and prototypes.
- **`view.inspect` / the selection**: a prim's variant selections, and for an instancer its points and prototypes.
- **Engine Map**: Compose's description names variant arcs; Extract's names point instancing.
- **Trace**: `CompositionMessage` for a selection naming a variant that isn't authored (as USD reports it); one message per instancer
  whose prototype has a LOD root (decision 4).
- Windows: none new; the LOD window is unchanged (point prototypes don't select levels).

## Verification

- **Composition**: `ngen-worldcheck` and `ngen-usdcheck` identical (`--transforms`, `--instancing`, `--instancers`) on `assets/layered/`, and
  on the test world with `R_2_2` and a tile in it at `full` (today 91,516 differing lines and 9 against 25 prototypes; after, none).
- **No variant warnings**: the test world packs with no "variants aren't read".
- **Draws**: at `dense_forest`, a tile at `full` draws its trees, rocks, bushes and ground cover; the draw count matches the manifest's
  instance and point counts for the tiles at `full` (per submesh); a screenshot shows them.
- **The rest unchanged**: the eight baselines byte-identical; Caldera's `ngen-worldcheck` against `ngen-usdcheck` at its default levels no
  worse than before (whatever variant sets it still authors now compose, so differences can only fall).

## Open questions

- Variant selections authored on an instance (the placement) and on the asset differ per placement here; does any test case need a
  selection authored inside a prototype's own nodes, which USD resolves within the prototype?
- `invisibleIds` with `ids` unauthored index points by position; is a scene that relies on that worth a test case?

## Deferred / follow-ups

- **Per-point LOD and GPU-side point expansion**: the instancer's arrays to the GPU, a level per point there. Trigger: meshlets (the
  roadmap's next vertical slice step), or a scene whose point-instanced prototypes carry LOD.
- **Variant fallbacks**. Trigger: a scene that relies on them.
