# 87. The engine takes the game engine industry's word where one exists

Date: 2026-11-15

## Status

Accepted

## Context

The engine's terms grew with its code. A review of [`GLOSSARY.md`](../../GLOSSARY.md) against the words game engine programmers use found
terms where the industry has a common word and ngen used another (*configuration value* for a cvar), and terms where ngen's word means
something else in the industry (*trace* for a log, when a trace is a timing capture; *draw* for an instance, when a draw is a draw call;
*entity* and *component* for composed prims and the data beside them, when both are gameplay words). Several of ngen's words also carried
more than one meaning inside the engine (*level*, *region*, *resident*, *slot*, *version*). ngen is built to be learned from and to be driven
by agents, and both read it with the industry's words in mind.

## Decision

Locked with Henrik on 2026-11-15:

- **The industry's word is the criterion.** Where the game engine industry has a common word for a thing, the engine uses it: in identifiers,
  file and folder names, records, RPC methods, console commands, windows and prose. What a rename costs orders the work and decides nothing.
- **Not every word of one engine.** The word is taken when it is common across engines or names the thing better, not because Unreal or
  Unity has it.
- **A kept word that differs gets a glossary note** naming the industry's. *Pack*, *packer*, *repack* and the pack nouns stay (assets are the
  system, packing the process), with the note that others call it cooking and mean an archive by a pack. `gamerelease` stays.
- **The renames made with this decision**: trace → log (severity for its level; the namespace is `logging`, since `log` is the C library's
  function); configuration value → cvar; verb → console command; draw and draw slot → instance and instance slot; culling region → draw
  list; the scene thread's batch → scene update; change list → scene changes; buffer view → view mode; asset version → content hash;
  selected or wanted level → desired level; frame slot → frame context; material key → material instance (and its key); view-local opinion
  → session opinion; engine light → default light; profiler site and lane → source location and track; World entity and component → prim and
  table; secondary output → sub-asset; render-LOD group → LOD group; the scene's resident state → ready.
- **A LOD level is always a LOD level.** The engine has no level in the sense of a map; that is a scene.

Rejected: keeping ngen's words with glossary notes only (the reader still has to translate every identifier, record and command); *cook* for
pack (the word is consistent across programs, commands and formats, and the earlier decision that separates the asset system from packing
stands); *streaming complete* for the ready scene (streaming never ends, the state comes and goes); a type `LodGroup` beside the class
`LodGroups` (too easily confused; `RenderLodGroup` reads as `RenderWorld`'s LOD group); `shipping` for `gamerelease` (not taken).

## Consequences

- Easier: a reader or an agent that knows engines finds `cvar.set`, the log, instances, draw lists and LOD groups under the names they expect.
- Easier: *level*, *region*, *resident*, *slot* and *version* each lost a meaning.
- Harder: every record field, method, console command and flag that was renamed breaks scripts written before 2026-11-15 (`wait-resident` →
  `wait-ready`, `config` → `cvar`, `introspect trace` → `introspect log`, the `scenethread`, `culling`, `lod`, `streaming`, `assets` and
  `profile` records' fields).
- Harder: plans, reviews and earlier records under `docs/` keep the words of their time, and their Verification sections name files and
  symbols as they were. The glossary is where the old word leads to the new.
- Accepted: *instance* now needs "USD instance" where both could be meant, and a World `PrimId` sits beside the view's `PrimHandle`.
- Open: the view still names some things by the old word for an instance (`SceneDraws`, `drawsOf`, the Draws columns, the records' `draws`
  counts), and a frame context's local variables and nested structs are still `slot`.

## Verification

- Sources: [plan_terminology.md](../plan_terminology.md); [`GLOSSARY.md`](../../GLOSSARY.md).
- Code: `src/log/` (`LOG_EVENT`, `logging::Severity`, `LogRing`, `LogStream`); `src/cvar/` (`cvar::Bool`), `src/view/cvarcommands.cpp`
  (`cvar.get`, `cvar.set`, `cvar.reset`); `src/rpc/consolecommands.h` (`ConsoleCommands`); `src/renderer/drawlists.h` (`DrawListWork`,
  `drawListCount`, `DrawLists::draw`); `src/renderer/renderworld.h` (`RenderMeshInstance`, `SceneChanges`); `src/renderer/passes/lightingpass.h`
  (`ViewMode`); `src/asset/assetclient.h` (`PackedAsset::hash`); `src/asset/pack/packer.h` (`writeSubAssets`, `subAssetPath`);
  `src/world/world.h` (`Prim`, `PrimId`, `World::desiredLevel`, `materialInstanceKey`); `src/profile/profile.h` (`SourceLocationInfo`,
  `TrackInfo`); `src/view/viewframe.h` (`SceneReadiness`, `updateReadiness`); `src/view/viewcommands.h` (`ReadyWaiter`, `LodGroupRequest`).
- Differs from plan: the namespace is `logging`, not `log`; a draw slot became an *instance slot*, not an instance index, as the code's word
  for the index is slot; the asset cache file keeps its `secondary` tag so caches written before still read; `DrawLists::drawRegion` became
  `DrawLists::draw`; the cvar `render.view.buffer` became `render.view.mode`; the LOD tool's `kind` value `render` became `group`.
