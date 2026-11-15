# Terminology: closer to the game engine industry's words

**Status. Draft.**

The glossary was reviewed against the words game engine programmers use. Where the industry has a common word and ngen used another, or where
ngen's word means something else in the industry, the engine takes the industry's word. This plan lists the accepted renames, the names each
becomes in code, records, methods, verbs and windows, and the order they land in. The industry's word is the criterion; the size of a rename
orders the steps and decides nothing.

## Current state

[`GLOSSARY.md`](../GLOSSARY.md) defines the terms. Counts below are occurrences in `src/`, `shaders/`, `build.cpp` and `pack.cpp`, measured on
`02469d6`, without `src/editor/`.

Decided in review:

| ngen term | Becomes | Why |
|---|---|---|
| Trace (`TRACE_EVENT`, `src/trace`) | log | In engines a trace is a timing capture (ngen's profile); a stream of categorised messages is a log |
| Level (trace) | severity | The log's `info` / `warning` / `error`; one meaning fewer for *level* |
| Configuration value | cvar | Quake, Source, Unreal, CryEngine |
| Verb | console command | The cvar's pair in every engine |
| Draw, draw slot | instance, instance index | A draw is a draw call; the GPU side already says instance |
| Region (culling) | draw list | One (view, bucket) list drawn with one indirect call; frees *region* for the test world and the RHI's copy regions |
| Batch (scene thread) | scene update | A batch is draw-call batching |
| Change list | scene changes | A changelist is a Perforce commit; the type is already `SceneChanges` |
| Buffer view | view mode | `VkBufferView` and glTF's `bufferView` are something else |
| Version (asset) | content hash | It is one; *version* keeps three meanings instead of four |
| Selected / wanted level | desired level | Two words for one thing; the industry's pair is desired and current (ngen's *drawn*) |
| Frame slot | frame context | Vulkan and D3D12 literature; one meaning fewer for *slot* |
| Material key | material instance (and its key) | One material with different input values is a material instance in Unreal, Unity and Godot |
| View-local opinion | session opinion | USD's session layer holds the same kind of opinion |
| Engine light | default light | The common word |
| Site (profile) | source location | Tracy's word |
| Lane (profile) | track | Perfetto's and Unreal Insights' word |
| Entity (World) | prim (`Prim`, `PrimId`) | In engines an entity is a gameplay object with components; ngen's is a composed prim |
| Component (World) | table | Reserved for entity components once gameplay lands |
| Resident (the scene) | ready | The level's *resident* is a decision by position, the scene's a completion; *ready* is the word a level already has for "waits for nothing" |
| Render LOD, render-LOD group | LOD group | The code's noun already; Unity's word for levels of several renderers switched by screen size |
| Secondary output | sub-asset | Unity's word for an asset inside another asset's file; USD has none. *Fragment* stays for the id's part after `#` |
| Level (of a LOD root) | LOD level, always in full | In the industry a level is a map; ngen has no level in that sense |

Kept, decided in review: **pack**, **packer**, **repack** (the process; `assets = system, pack = process` stands), the nouns **layer pack**,
**mesh pack** and **material pack** (a pack is what packing produces), and **`gamerelease`**.

## Scope

In:

- The renames in the table: identifiers, file and folder names, macros, record names and fields, RPC methods, verbs, command-line words, window
  titles, config value names, shaders, library READMEs, `README.md`, `REFERENCE.md`, `GLOSSARY.md`, `AGENTS.md` and the skills under
  `.claude/skills/`.
- Glossary notes that name the industry's word where ngen keeps its own: *pack* ("cooking elsewhere"; a pack is one file, not an archive), and
  a streaming root's coarse levels ("HLODs elsewhere").
- The "Words with more than one meaning" table brought up to date: rows that lose a meaning, the new *log* row (the log, the build log,
  `AssetClient::log`), and *batch* (`UploadBatch`, the log stream's batches).

Out:

- `docs/`: plans, reviews and ADRs keep the words they were written in. They are history.
- `src/editor/`: unchanged until `ngen-editor` is built from it.
- `external/`.
- Words not accepted in review: **pack**, **packer**, **repack**, **`gamerelease`**, **meshlet pack**, **vertical slice**.
- Behaviour. No step changes what the engine does; a step that needs a behaviour change to land is wrong.

## Steps

One step per term, smallest first, each a change of its own that builds, formats and passes Verification before the next starts. A step renames
the term everywhere in scope at once: no step leaves both words in the code.

1. **Prose only.** `GLOSSARY.md`, the library READMEs:
   - View-local opinion → session opinion (2 occurrences in `src/`).
   - Change list → scene changes in comments and READMEs (16); `changeListsSubmitted` → `sceneChangesSubmitted`, `changeListsDrawn` →
     `sceneChangesDrawn`, and the record fields that carry them.
   - Material key → material instance; `materialKey` and `parseMaterialKey` → `materialInstanceKey`, `parseMaterialInstanceKey` (9).
   - Engine light → default light; `/__DefaultEngineLight` → `/__DefaultLight` in `src/view/placement.cpp`.
   - The *pack* note; under **Cell**, that a streaming root's coarse levels (the test world's region `proxy`, tile `simplified`) are what the
     industry calls HLODs.
   - "LOD level" written in full where the glossary and the READMEs say "level" for one, and under **LOD level** that ngen has no level in the
     sense of a map. *Level* stays in identifiers (`wantedLevel`, `residentLevels`, `lodGroupLevel`).
   - The multi-meaning table, with a *residency* row: a LOD level's residency (asked for by position), a LOD group's
     residency mask (`src/renderer/lodgroups.h`: a level's meshes in the geometry pool).
2. **Batch → scene update.** `SceneBatch` → `SceneUpdate` in `src/view/viewscene.*`, its fields and the `Scene` events that name a batch;
   `src/view/README.md`.
3. **Log severity.** `trace::Level` → `Severity` (`src/trace/traceevent.h`), `levelName` → `severityName`, the stream's `"level"` field →
   `"severity"` (`src/trace/tracestream.cpp`), its readers in `src/introspect/introspecttrace.cpp` and `introspectwindow.cpp`, the
   `introspect trace` filter option. Lands before step 12 so that step is a pure rename.
4. **Desired level.** `World::wantedLevel` → `desiredLevel`, `lodWanted` and the LOD uses of `wanted` and `selected` in `src/world`,
   `src/view/placement.cpp`, `src/view/viewscene.*`, `src/tools/lodtool.*`, `streamingtool.*`; the `wanted` and `selected` fields of the `lod`
   and `streaming` records (`src/tools/toolcommands.cpp`); the LOD and Streaming windows' columns. *Selected* stays for the selection, *wanted*
   for buffer growth and frame-graph passes.
5. **View mode.** `GBufferView` → `ViewMode`, `gbufferViewMode` and the UI's `gbufferView` → `viewMode`, `renderconfig::bufferView` →
   `renderconfig::viewMode`, the configuration value `render.view.buffer` → `render.view.mode`. The `view` verb and `--view` flag keep their
   names: they set the view mode.
6. **Content hash.** In `src/asset`: the stream's `version` field → `hash`, `AssetClient`'s and the server's members, the Assets window's
   column, the cache record. A pack rule's `version` keeps its name. The server and the client change together; a cache written before the
   change is repacked.
7. **Source location and track.** `SiteInfo` → `SourceLocationInfo`, `registerSite` → `registerSourceLocation` (56), `LaneInfo` → `TrackInfo`,
   `lane*` → `track*` (about 350) in `src/profile`, the profiler window in `src/ui`, the `profile` record's fields.
8. **Frame context.** `frameSlot` → `frameContext`, `frameSlots` → `frameContexts` (116 identifiers, 50 in prose) across `src/renderer`,
   `src/rendertools`, `src/rhi/examples/common`; "one slot cycle late" → "one cycle of frame contexts late"; `src/renderer/README.md`,
   `src/rhi/README.md` ("Frame pacing").
9. **Draw list.** `RegionWork` → `DrawListWork`, `region*` → `drawList*` (`regionCount`, `regionWork`, `regionOffsets`, `cullRegions`,
   `drawRegion`, `regionDraws`, …; about 500 with the shaders) in `src/renderer` and `shaders/`; the `culling` record's fields; the GPU Scene
   window. The RHI's `srcRegion`, `dstRegion`, `RhiBlitRegion` and `copyBufferRegions` keep their names: they are Vulkan's copy regions.
10. **Instance.** Draw → instance, draw slot → instance index: `drawSlots` → `instances`, `slotsAdded` / `slotsRemoved` → `instancesAdded` /
    `instancesRemoved` in `RenderWorld` and `SceneChanges`; prose in `src/renderer/README.md` and `src/view/README.md`. `DrawRecord`, the draw
    log and `indirectDraws` keep their names: each is about a draw call. A USD instance is written "USD instance" wherever both could be meant.
11. **cvar and console command.** `src/config/` → `src/cvar/` (`cvar.h`, `cvar.cpp`), `config::Bool` / `Int` / `Float` / `Enum` / `Vec3` /
    `Value` / `Type` → `cvar::*` (about 150 declarations), the `*config.cpp` / `*config.h` files that declare them (`renderconfig`,
    `toolsconfig`, `viewconfig`, `profileconfig`, `uiconfig`) → `*cvars.*`, the `config` record → `cvars`, `config.get` / `config.set` /
    `config.reset` → `cvar.*`, the `config` verb → `cvar`, the Configuration window → Cvars (`configwindow.*` → `cvarwindow.*`). The build
    system's `configuration` and `-c` keep their names. `RpcVerbs` → `ConsoleCommands`, `rpcverbs.*` → `consolecommands.*`, "verb" → "console
    command" in prose and in `rpc.describe`'s output. The glossary's **Command** entry goes; the `*commands.cpp` files keep their names.
12. **Log.** `src/trace/` → `src/log/` (`log.h`, `logevent.h`, `logring.*`, `logstream.*`), `TRACE_EVENT` / `TRACE_WARNING` / `TRACE_ERROR` →
    `LOG_EVENT` / `LOG_WARNING` / `LOG_ERROR` (140 sites), `TraceRing` → `LogRing`, `TraceStream` → `LogStream`, the methods `trace.events`,
    `trace.subscribe`, `trace.unsubscribe` → `log.*`, `./ngen-cli introspect trace` → `introspect log`, the introspect window's Trace tab, the
    `trace` target in `build.cpp`, `AGENTS.md` ("Verifying changes"), the `run-headless` skill.
13. **Prim and table.** `Entity` → `Prim`, `EntityId` → `PrimId`, `noEntity` → `noPrim`, `VisitedEntity` → `VisitedPrim`, `EntityTransform` →
    `PrimTransform`, `visitEntity` → `visitPrim` (about 350) in `src/world`, `src/view`, `src/apps/worldcheck.cpp`. `PrimHandle`
    (`src/scene/scenehandles.h`) keeps its name; `src/world/README.md` and the glossary say which is which: a `PrimId` is a prim's index in a
    World content, a `PrimHandle` a drawn prim's index in the view's library. "Component" → "table" in `src/world/README.md`, the glossary and
    `screenshots/world_overview.svg`.
14. **Sub-asset.** `writeSecondaryOutputs` / `readSecondaryOutputs` → `writeSubAssets` / `readSubAssets`, `secondaryOutputsList` →
    `subAssetsList`, `secondaryOutputFile` → `subAssetFile` in `src/asset/pack/packer.*`, `src/asset/server/assetcache.*`, `assetserver.*`,
    `src/apps/packerusd.cpp`; "secondary output" → "sub-asset" in `src/asset/README.md`. The `fragment` parameter and `<file>#<fragment>` keep
    their names.
15. **LOD group.** "Render-LOD group" → "LOD group", "render-LOD root" → "a LOD root placed as LOD groups", "Render LOD" as a heading → "LOD
    groups" (about 150 places in comments and READMEs). The method `view.renderlod` → `view.lodgroups`, the record `renderlod` → `lodgroups`,
    `RenderLodRequest` (`src/view/viewcommands.h`) → `LodGroupRequest`, `renderLodRequests` → `lodGroupRequests`, `serviceRenderLodRequests` →
    `serviceLodGroupRequests`, `LodToolState::RenderLod` → `LodGroups`. The renderer's types keep their names: `RenderLodGroup` and
    `RenderLodKind` are `RenderWorld`'s parts as `RenderMeshInstance` and `RenderLight` are, `GpuLodGroup` is the GPU's form, `LodGroups` the
    table. No type named `LodGroup` is added.
16. **Scene ready.** The scene's *resident* → *ready*: the `SceneResident` event → `SceneReady`, `ResidentWaiter` → `ReadyWaiter`,
    `residentWaiters` → `readyWaiters`, `answerResidentWaiters`, `waitForResident` → `waitForReady`, `everResident` → `everReady`,
    `lastResident*` → `lastReady*`, `residentWhy` / `residentReason` / `residentText` → `notReady*`, `g_resident`; the method
    `view.waitResident` → `view.waitReady`, the console command `wait-resident` → `wait-ready`, the flag `--no-wait-resident` →
    `--no-wait-ready`; the `scene` record's `resident`, `everResident` and `lastResident` fields; `src/view/README.md`, `AGENTS.md`, the
    `run-headless` skill. A LOD level's residency keeps every name it has: `residentLevels`, `setResidentLevels`, `residentBytes`,
    `residentSets`, `updateResidency`, `ViewResidency`, `ResidencyChanged`, the Streaming window's Resident column. The glossary says the two
    apart: a LOD level is *resident* when the camera's position asks for it, *ready* when its placement waits for nothing; the scene is
    *ready* when every resident level is ready, uploaded and drawn.
17. **Land.** Status, `docs/README.md`, and an ADR for the rule: the engine takes the game engine industry's word where one exists, and a kept
    word that differs gets a glossary note naming the industry's.

## Visibility

Nothing is added: no record, method, verb or window. What an agent or a script reads changes name:

- Methods: `trace.events`, `trace.subscribe`, `trace.unsubscribe` → `log.*`; `config.get`, `config.set`, `config.reset` → `cvar.*`;
  `view.renderlod` → `view.lodgroups`; `view.waitResident` → `view.waitReady`.
- Command line and console commands: `introspect trace` → `introspect log`; `config` → `cvar`; `wait-resident` → `wait-ready`;
  `--no-wait-resident` → `--no-wait-ready`.
- Records: `config` → `cvars`; `renderlod` → `lodgroups`.
- Log events: `SceneResident` → `SceneReady`.
- Record fields: the log's `level` → `severity`; `lod` and `streaming` records' `wanted` / `selected` → `desired`; the asset records' `version`
  → `hash`; the `culling` record's region fields → draw list; the `profile` record's lanes and sites → tracks and source locations; the scene
  counters' `changeLists*` → `sceneChanges*`; the `scene` record's `resident`, `everResident` and `lastResident` → `ready`, `everReady` and
  `lastReady`.
- Configuration values: `render.view.buffer` → `render.view.mode`.
- Windows: the introspect window's Trace tab → Log; column and label texts in the LOD, Streaming, Assets, GPU Scene and profiler windows; the
  Configuration window → Cvars.

`REFERENCE.md` lists every one of these and changes in the same step as the code. Scripts kept outside the repository that read the renamed
fields break; that is accepted.

## Verification

For every step:

- `./ngen-cli build` and `./ngen-cli build examples` succeed, and `./ngen-cli build format` changes nothing.
- `rg` for the step's old identifiers in `src/`, `shaders/`, `build.cpp`, `pack.cpp`, `README.md`, `REFERENCE.md`, `GLOSSARY.md`, `AGENTS.md`
  and `.claude/` finds nothing outside `src/editor/` and the kept names the step lists.
- A headless run of `assets/three_cubes.usda` and of `assets/layered/root.usda` with the log captured has no warnings and no errors.
- `--screenshot` of both scenes at a fixed `--camera` is byte-identical to the one taken before the step.
- `ngen-worldcheck` fingerprints of both scenes are identical to the ones taken before the step.
- The records the step renames, written with `record <name> <file>` at frame 120, equal the ones taken before the step once the renamed keys
  are mapped back.
- The RHI examples pass `--check --validation`.

For the plan: every row of the table in "Current state" reads the same in `GLOSSARY.md`, the code and `REFERENCE.md`.

## Decisions

Locked in review:

1. **Entity → prim.** It is what USD calls a composed prim, and it leaves *entity* to gameplay. `PrimId` and `PrimHandle` sit side by side;
   the alternative, *node*, is USD's word for a prim index's nodes and was not taken.
2. **Component → table.** The industry has no word for data beside the prims; the aim is to leave *component* free for gameplay.
3. **cvar all the way**: the code, the record, the methods, the console command and the window, not only the prose. One word for one thing, and
   *config* then means only a build configuration.
4. **Verb → console command**, with `RpcVerbs` → `ConsoleCommands`. The glossary's **Command** entry goes.
5. **An ADR at landing** for the naming rule.
6. **The scene is *ready*, not *resident*.** *Streaming complete* names an end, and streaming never ends; *loaded* and *streamed in* sit too
   close to a LOD level's residency. *Ready* extends the word a level already has.
7. **LOD group in prose and commands only.** A type `LodGroup` beside the class `LodGroups` would be confused with it, and `RenderLodGroup`
   already reads as `RenderWorld`'s LOD group.
8. **Layer, mesh and material pack stay**, with *pack*.

## Deferred / follow-ups

- **The test world's level names.** Region `proxy` shares its name with USD's `proxy` purpose, which the engine also draws
  (`scene.purposes.proxy`), and both `proxy` and tile `simplified` are HLODs. Renaming them to `hlod` is a change in the test world's own
  repository, not in ngen: the engine reads no level name. Trigger: the next time the world is generated again.
- **The glossary's Cell entry against the test world.** The entry lists two levels per cell (`full` and `proxy`, `full` and `simplified`); the
  test world's design lists three, with `medium`. Check against the generated world when step 1 touches the entry.
