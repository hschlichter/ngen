# 52. ngen-view renders layer packs and links no OpenUSD

Date: 2026-10-12

## Status

Accepted

Supersedes [ADR 6](0006_usd_stage_is_the_scene.md)

## Context

With the USD stage as the view's scene ([ADR 6](0006_usd_stage_is_the_scene.md)), start-up was USD's: Sponza took 2.6 seconds to open the stage
and 5.4 seconds to its first frame. Caldera has 63,700 layers and 2.85 million placements and needs streaming. The tool architecture
([ADR 34](0034_process_split.md)) already separated the editor, the asset server and the view; packing had to be request-driven and cacheable
([ADR 42](0042_separate_asset_server.md)), and the runtime should not depend on pxr.

## Decision

- `ngen-packer-usd` packs each USD file in a job of its own. It reads that file's opinions with `Sdf`, with no composition across files, into a
  **layer pack** holding the attributes the engine uses. Each mesh and material the file defines becomes its own asset, `<file>#<prim path>`.
- One job has several outputs, cached as one group.
- The pack format is hand-written binary: an `NGPK` header with a kind and a format version, a string table and sections. No serialization
  library.
- There is one pack kind per file. A "sub-pack" is simply a layer pack used through a reference or payload.
- The view requests what a pack names; the server never chains jobs. There is no in-memory packing fast path: the editor and the view never pack.
- ngen-view composes the packs itself ([ADR 53](0053_worldcomposer_composes_as_pcp.md)) and links no OpenUSD. USD editing leaves the view until
  `ngen-editor` exists ([ADR 58](0058_editor_process_owns_usd.md)). Picking moves to the GPU, replacing the CPU BVH.

## Consequences

- Easier: the view is free of pxr and starts without USD's cost.
- Easier: pack jobs are self-contained per file, so they cache well and could be distributed.
- Easier: packs carry what streaming needs — payload placements, `extentsHint`, LOD roots.
- Harder: there is no USD editing at all until the editor process exists.
- Harder: arcs the composer doesn't support are reported, not composed: inherits and specializes across files, relocates, collection bindings.
- Harder: a view always needs a running asset server.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_scene_pack.md](../plan_scene_pack.md) (decisions 1 to 14), [plan_tool_architecture.md](../plan_tool_architecture.md)
  (decisions 5, 6, 9 and "Two edit paths"), [src/asset/README.md](../../src/asset/README.md).
- Code: `pack.cpp:29` (the `usd` rule, `ngen-packer-usd`, version 9), `src/asset/pack/scenepack.h:12` (the `NGPK` layout),
  `src/asset/pack/usd/usdpacker.cpp:927` (`SdfLayer::FindOrOpen`), `build.cpp:173` (the `world` library, "No OpenUSD"), `build.cpp:438`
  (ngen-view links no `-lusd*`), `src/view/viewpicking.h:10` (picking on the GPU).
- No file under `src/view`, `src/scene` or `src/renderer` mentions pxr; `src/world/worldcomposer.cpp` does only in a comment.
- Differs from plan: [plan_tool_architecture.md](../plan_tool_architecture.md) decision 6 says the packer resolves the rest of USD composition; in
  the code the packer composes nothing across files — `WorldComposer` does. The packer does use a stage of the file alone for in-file composition.
  The engine libraries linking no pxr is a convention of `build.cpp`, not enforced.
- Rejected alternatives: one job per mesh (Sponza's 437-megabyte file read once per mesh); the view keeping pxr until the editor split; a CPU
  spatial index for picking.
