# Caldera to UsdLod

**Status. Landed.**

A converter that rewrites Caldera's LOD into the UsdLod schema, so the engine reads one LOD convention, UsdLod, and nothing in it knows how
Caldera's authors expressed level of detail. Caldera is the large-scene test for streaming and LOD ([plan_streaming_lod.md](plan_streaming_lod.md)).
The converter and the converted data live in a fork of [Activision/caldera](https://github.com/Activision/caldera), not in ngen.

## Current state

Caldera (`~/Code/caldera`, a clone of `Activision/caldera`; 63,700 binary `.usd` layers, 9.7 GB, tracked in git directly) expresses LOD as two
variant sets. Measured with a throwaway probe against OpenUSD v26.08 (the numbers are in [plan_streaming_lod.md](plan_streaming_lod.md),
"Current state"):

| Variant set | Authored in | Variants | Each variant is | Roots |
|---|---|---|---|---|
| `districtLod` | `map_source/mp_wz_island_geo.usd` | `full`, `proxy` | `full`: a **reference** to `prefabs/br/wz_vg/mp_wz_island/season_4/<district>.usd`; `proxy`: a **payload** to `assets/xmodel/generated_proxies/<district>_proxy.gdt.usd` | 45 (districts and terrain tiles, `st_*`) |
| `modelLod` | a model asset, `assets/xmodel/**/<model>.gdt.usd`, an **instanceable** prim | `lod0` … `lod3`, not all present | a **payload** to `<model>_lod<N>.geo.usd` | 8,073 model assets |

- A variant body holds one arc and an `extentsHint`, nothing else; no switch distances are authored anywhere.
- Levels have gaps (`[lod0 lod3]`) and duplicates (one model's `lod1` and `lod3` both point at `_lod3.geo.usd`).
- `caldera.usda` selects `districtLod = proxy` on every district through `over`s; every model selects `lod0`, the finest.
- **UsdLod** (`pxr/usd/usdLod/`, OpenUSD v26.08): the `LODRootAPI` schema (C++ `UsdLodRootAPI`) on a prim makes its *child prims* the
  levels, in child order, index 0 the finest; `rel lod:heuristics` targets heuristic prims (`LODScreenSizeHeuristic`, `LODDistanceHeuristic`)
  with a `lod:domain`; `int lod:default:index` is the fallback; `LODOverrideAPI` overrides descendants. A screen-size heuristic's `thresholds`
  are screen fractions in descending order, one per level switch (`[0.25, 0.10, 0.025]`: level 0 above 25%, level 3 at 2.5% or less). Unlike
  a variant set, all levels are composed at once.

**The converter's scan** (`convert.py scan`, 7.8 s over the 63,700 layers; nothing changed) found:
- 8,076 layers to rewrite: one with the 45 district roots, 8,073 model assets with one root each. No variant body holds anything but its arc
  and `extentsHint`, so none is skipped.
- Model roots by number of levels after merging: 1 level 5,510, 2 levels 1,729, 3 levels 458, 4 levels 376; 263 duplicate levels merged.
- Every model root has one other child, an empty `mtl` Scope (Caldera ships no materials); it stays next to `lod`.
- The only selections authored outside their root are `caldera.usda`'s 45 `districtLod = proxy`; no placement selects a model level.
- No opinion from outside a district's variants reaches its content (each district composed with no variant selected has no children), so
  moving the content under `<district>/lod/lod<i>` loses nothing.

## Scope

**In**
- A converter in the Caldera fork that rewrites every `districtLod` and `modelLod` variant set into a UsdLod root, in place on a branch.
- A screen-size heuristic per root, from its `extentsHint`, with generated thresholds.
- A check that the converted data composes to the same geometry as the original, level for level.

**Out**
- Anything in ngen's engine: the packer reading UsdLod and the view selecting levels are the streaming steps
  ([plan_streaming_lod.md](plan_streaming_lod.md), Steps).
- Tuning the thresholds against what the engine draws (needs the engine side first).
- Materials and textures for Caldera (roadmap, Caldera: "Shading without textures").
- Physics and audio LOD domains (the collision and volume prims stay as they are).

## Decisions

1. **A converter that rewrites the layers, not a layer over the map.** Locked. A layer over the map could reach the 50 district and tile roots,
   but not `modelLod`, which lives inside instanceable model assets that no outside opinion can change.
2. **In place, on a branch of the fork.** Locked by the fork: git keeps the original, and the converted state is a commit, so others get it by
   checking out the branch. Every layer is rewritten in its own format (`usdc` stays `usdc`).
3. **Every level's arc becomes a payload.** Locked: required by UsdLod. A variant set composes only the selected variant; UsdLod composes all
   children, and a reference always composes. Kept as a reference, every district's `full` would compose on open (1.4 million prims for
   `map_capital` alone). As payloads, a level composes only when loaded, so the coarsest-only load is as light as today's default.
4. **The root on a new child, `lod`.** Locked. UsdLod treats every child of a root as a level, so the root can't be a prim that has, or
   may later get, other children. `<prim>/lod` gets `UsdLodRootAPI`; its children `lod0` … `lodN` are the levels; the heuristic is a sibling,
   `<prim>/lodHeuristic`, since a child of the root would count as a level. Alternative: the root on the prim itself, which keeps paths
   shorter but breaks on the first non-level child.
5. **Levels in order, gaps closed, duplicates merged.** Locked. `full` then `proxy`; `lod0` … `lod3` by name. Consecutive levels whose arcs
   name the same file and prim become one level; a gap (`[lod0 lod3]`) gives two levels. The original variant name stays on each level as
   `customData` (`calderaVariant`), so a level can be traced back.
6. **Heuristic: one `LODScreenSizeHeuristic` per root, imaging domain**, `extent` from the root's `extentsHint`, `thresholds` generated as
   halvings from a quarter of the screen: `[0.25, 0.125, 0.0625]`, cut to one fewer than the root's levels (a two-level district gets `[0.25]`).
   District and model levels get the same rule. Locked as the starting point; the numbers are tuned once the engine draws levels.
7. **`lod:default:index` is the coarsest level.** Locked: a consumer that ignores the heuristics shows the cheap level, as Caldera's own default
   (`proxy`) does.
8. **Python with `usd-core` 26.8.** Locked: the fork already ships a Python example (`caldera.py`), `usd-core` 26.8 on PyPI has UsdLod and a
   wheel for the system's Python 3.14, and the work is layer-level (`Sdf`) edits. It runs from a virtual environment in the fork
   (`tools/usdlod/.venv`, ignored by git); nothing is installed system-wide.
9. **A model with one level gets no LOD root.** Locked. 5,510 of the 8,073 model roots have only `lod0`. Its payload moves onto the model prim
   itself, as it composed with `lod0` selected; a UsdLod root always means there is a choice. The 45 districts and the 2,563 models with two
   or more levels get roots.
10. **An instanceable root keeps its instancing through a class prim.** Locked, after the first full run: a prim is only an instance if it
    has a composition arc of its own, and the variant set was that arc. With the levels' payloads moved onto `lod/lod<i>`, 63,938 placements
    in `map_capital` stopped being instances (139,137 became 75,199). The structure of an instanceable root (`lod`, its levels, `lodHeuristic`)
    is authored on `</_usdlod/<model>>`, a `class` prim in the same layer, and the root references it: the root keeps an arc, so it stays an
    instance, and the composed paths are unchanged. Rejected: a separate file per model (2,563 new files, same effect), and instancing each
    level instead of the model (every placement's `lod` and `lodHeuristic` composed per placement, about 320,000 more prims in `map_capital`).

## Steps

All files are in the Caldera fork, on a branch `usdlod`.

1. **`tools/usdlod/convert.py`** (or the C++ equivalent): walk every `.usd`/`.usda` layer; open it with `Sdf.Layer.FindOrOpen` (no composition).
   For each prim spec that authors `districtLod` or `modelLod`:
   1. Read the variants in order: each variant body's single arc (reference or payload: asset path, prim path) and its `extentsHint`. A body
      with anything else (children, other properties) is not converted; it goes into the report.
   2. Merge consecutive levels with the same arc.
   3. Author `<prim>/lod` (typeless `def`) with `prepend apiSchemas = ["LODRootAPI"]`, `lod:default:index` = the last level, and
      `lod:heuristics` targeting `<prim>/lodHeuristic`; author `<prim>/lod/lod<i>` (typeless `def`) with the level's arc as a payload and
      `customData = {calderaVariant = …}`.
   4. Author `<prim>/lodHeuristic` (`def LODScreenSizeHeuristic`) with `lod:domain = "imaging"`, `extent` from the `extentsHint`, and the generated
      thresholds.
   5. Remove the variant set and the prim's own selection for it.
2. **Remove the selections elsewhere**: every `variants = { districtLod = … }` / `modelLod = …` opinion in any layer (`caldera.usda`'s `over`s
   among them), since the sets no longer exist.
3. **Report** (`tools/usdlod/report.json`): roots converted per set, levels per root (histogram), merged duplicates, skipped specs with the reason,
   layers rewritten, run time.
4. **`tools/usdlod/verify.py`**: the equivalence and load checks under Verification, runnable on its own.
5. **`tools/usdlod/README.md`**: what the conversion does, how to run it and verify it, what changed in the data. One commit for the tool, one
   for the converted data, so the data commit can be regenerated.

**As built** (branch `usdlod` of the fork, [hschlichter/caldera](https://github.com/hschlichter/caldera): commit `8c267c47a` the tool,
`0daa744de` the converted data; the converter's README, `tools/usdlod/README.md`, has the layout and how to run it):
- `convert.py run` rewrote 8,076 of the 63,668 layers in 8.4 s: 45 district and tile roots, 2,563 model roots (2,315 instanceable, through
  `_usdlod`), 5,510 models turned into a single payload, 263 duplicate levels merged, 45 selections removed; nothing skipped.
- Two models whose variants all name the same file merged into one level and got a single payload, by decision 9.
- `verify.py after`: PASS. The default map and `map_capital` in full compose the same prims, meshes and points path for path (1,935,073 prims,
  596,867 meshes, 141,852,999 points for `map_capital`, instance proxies included), with the same instance counts (85; 139,137). 200 sampled
  models match at every level. 2,608 roots valid; no LOD variant set or selection left. A second `convert.py run` changes nothing.
- `map_capital` has 140 fewer prototypes (2,675, from 2,815): different model assets that point at the same geometry files (a light's `off`,
  `on_cool` and `on_warm` assets) now share one, because their instances compose the same payload; before, each asset's variant bodies made
  its prototype distinct. Checked by grouping the instances by prototype before and after: 97 new groups, each a union of old groups.

## Verification

- **Nothing left behind**: no layer authors a `districtLod` or `modelLod` variant set or selection; the number of `LODRootAPI` prims equals the
  number of converted specs in the report (45 district and tile roots, and one per model asset with more than one level).
- **Instancing kept**: the same number of instances in the default map and in `map_capital` in full.
- **Same geometry, level for level**: for `map_capital` and a sample of 200 models, the original composed with variant *X* selected and the
  converted composed with only level *X*'s payload loaded have the same meshes, the same mesh points and the same world bounds (the added
  `lod`, `lod<i>` and `lodHeuristic` prims aside).
- **Light by default**: `caldera.usda` opened with only each root's coarsest level loaded (a payload load rule) composes the same meshes and
  points as the original's default (every district at `proxy`): 19,044 meshes and 14.6 M points; it opens in the same time (142 ms today).
- **Rerunnable**: running the converter on its own output changes no layer.
- **Readable by the schema**: `UsdLodRootAPI` is applied and `UsdLodScreenSizeHeuristic` is valid on every converted root, and each root's
  heuristic is found through `lod:heuristics`; the `thresholds` have one entry fewer than the root's levels.

## Open questions

- **Thresholds**: the `0.25 / 2^k` rule is a guess; the right numbers come from looking at the engine's output.
- **Publishing**: the licence is non-commercial; whether the fork, with the converted data, is public is for you to decide.

## Deferred / follow-ups

- **Upstream**: offer the converter, or the converted branch, to the Caldera maintainers. Trigger: the conversion is verified and in use.
- **A distance heuristic next to screen size.** Trigger: the engine needs distance-based selection (physics, audio).
- **Physics-domain roots for the collision volumes.** Trigger: physics (roadmap, Engine features).
