# Tool architecture: RPC, packs, build server, editor, introspection tool

**Status. Draft.**

Umbrella plan: the target architecture in four steps. Each step becomes its own `docs/plan_<topic>.md` when it is picked up. The goal is to reach the
architecture in few steps; each step lists what it knowingly leaves missing. It supersedes [plan_async_asset_system.md](plan_async_asset_system.md).

## Where we want to get to

- **The engine knows nothing about USD's API.** ngen-view consumes only packed, engine-ready data. It knows four USD-derived concepts: layer packs,
  sub-packs, variant sets and typed components. Details under "What ngen-view knows".
- **The editor owns USD.** `ngen-editor` holds the composed stage, the layers, editing and undo. It doesn't pack. Changes it authors go to the build
  server, and ngen-view receives the packed result as a delta against what it has loaded.
- **Interactive transforms start in the view.** ngen-view applies a gizmo drag at once and sends the transform to the editor, which authors it into
  the edit layer so it can be saved.
- **The build server packs.** It is `ngen-build --serve`, and the packer runs only there: USD scenes, meshes, textures, materials, and later physics and
  LOD. ngen-view and every other tool request packs asynchronously and are told when a result is ready.
- **One RPC system** connects the engine, the editor, the build server, the introspection tool and agents. Agents use it to query state and data and to
  drive the engine and the editor.
- **The introspection tool** (`ngen-introspect`) is where data is captured and examined in depth.
  - It has its own window and shows data queried from a running ngen-view.
  - ngen-view connects to it when it's open, to send trace data and to answer its requests.
  - Windows about the view's own live frame stay in ngen-view (Decision 8).

## Current state

- **One process does everything.** ngen-view contains the engine, the viewer, the editor UI (scene tree, properties, layers, undo, asset browser,
  gizmos) and every introspection window: Memory, Capture, Frame Debugger, GPU Scene, Counters, Debug View, Frame Graph, Render Debug, Performance,
  Camera and Culling.
- **The scene is the USD stage, in-process.**
  - `USDScene` (`src/scene/usdscene.*`) composes the stage and authors session-layer defaults such as the fallback distant light.
  - `SceneUpdater` applies edits with undo.
  - `USDRenderExtractor` produces `RenderWorld`, `MeshLibrary` and `MaterialLibrary`, and `Renderer::uploadRenderWorld` consumes them.
- **The renderer's inputs already have no USD in them**; only comments mention it. The coupling sits in `src/scene/`, `src/main.cpp` and the editor
  windows in `src/ui/`.
- **Only transforms update incrementally.** Anything else re-uploads geometry, textures and tables in full (`src/renderer/README.md`, Known gaps).
- **Nothing is packed.** USD, meshes and PNGs load and decode at runtime; only shaders are compiled by the build.
  [plan_async_asset_system.md](plan_async_asset_system.md) proposed a cook cache and CLI beside the build system. This plan replaces that with packing
  on the build system (`notes.md`).
- **No live access.** Agents use flags, `--script` verbs, `--obs-output` JSONL and dump files. There is no networking code.
- **Builds are one-shot.** `ngen-build` exits after each build, and `ngen-cli` is a front door over it.

## What ngen-view knows

The packer resolves USD into four things the view understands:

- **Layer packs.** Every USD layer in the stage's layer stack packs into its own layer pack.
  - A layer pack holds that layer's opinions: component fields per entity, keyed by stable entity id.
  - The view composes the stack with one fixed rule: an ordered list, and per field the strongest layer that has an opinion wins.
  - Muting, unmuting or reordering a layer is a change to the stack. The view applies it without a repack.
  - An edit in layer X changes only X's pack.
- **Sub-packs.** A USD reference or payload target packs once, into a sub-pack. Every place that references it is an instance of that sub-pack, so a
  building referenced 50 times is one sub-pack. Payload boundaries are the load unit: a sub-pack loads when needed.
- **Variant sets.**
  - The packer packs every variant of a variant set as alternative opinions under that set.
  - The view composes the selected variant, and switching a selection is a view-side change, applied without a repack.
  - The cost is pack size: every variant is packed even when only one shows.
  - Why live rather than packer-resolved: the runtime can switch variants (a door from `closed` to `broken`, a paint colour) without the editor or
    a repack. The packer-resolved alternative is simpler for the view, because switching would be editor → repack → delta. It was considered and not
    chosen.
- **Components.** An entity is a stable id plus typed components:
  - first: transform (local, with a parent link), mesh reference, material reference, light, camera, visibility
  - later: physics body and collider, LOD group, and others

  The packer maps USD schemas to components, for example `UsdGeomXformable` to transform, `UsdLux` to light, `UsdPhysics` to physics components. The
  view knows component types, never schemas. Components are versioned, and a component the view doesn't know is skipped, so the packer can add
  schemas ahead of the view.

**Resolved by the packer, invisible to the view:**
- inherits and specializes
- prim types and asset resolution
- session-layer defaults such as the fallback light and up axis, which are authored into the packed scene

**Consequences of live composition in the view.**
- A layer or a variant can override the local transform of any entity, including a parent. So the view keeps a transform hierarchy (parent links and
  local transforms) and resolves world transforms itself. The packer can't bake world transforms.
- The view composes in the same order USD does for these arcs: the layer stack, with variant opinions applied at their position in the stack.

**Stable entity ids** come from the packer: the prim path, and the submesh index for meshes split by material. They stay the same across repacks, so a
delta can name an entity. The prim path also travels as a debug name the view only prints. The view reports picks and transform edits by entity id,
and the editor maps them back to prims.

**Assets** (meshes, textures, materials, shaders) are packed chunks. Their id is the project-relative source path, plus a `#` fragment for things
packed out of one file, as USD does it. The content hash of the packed bytes is the id's version. "Is this loaded already, and in which version?" is a
lookup in the view's manifest.

## Architecture

```
   ┌──────────────────────────┐   ┌─────────────────┐   ┌─────────────────┐
   │ ngen-editor              │   │ ngen-introspect │   │ agents          │
   │ USD stage, layers, undo  │   │ all introspect- │   │ via ngen-rpc    │
   └──┬─────────────────▲─────┘   │ ion windows     │   └────────┬────────┘
      │ authored edits  │ picks,  └────────┬────────┘            │
      │ to repack       │ transform edits  │ queries, trace      │ queries, control
      ▼                 │                  ▼                     ▼
   ┌─────────────────────┐        ┌───────────────────────────────────────────┐
   │ ngen-build --serve  │        │ ngen-view (no USD)                        │
   │ build graph,        │──────► │ layer stack, sub-packs, variants,         │
   │ packers, pack cache │ deltas │ components; manifest of what is loaded;   │
   │ (content hashed)    │        │ gizmos applied locally                    │
   └─────────────────────┘        └───────────────────────────────────────────┘
     every process runs one rpc endpoint; connections are symmetric
```

### Two edit paths

- **Authored in the editor** (property window, layer operations, adding prims, swapping a texture):
  - The editor authors the change into its in-memory stage; nothing is written to disk until a save.
  - Every ngen-view showing that scene receives the result as a delta and applies it on arrival.
  - **How an unsaved edit becomes packed data is not decided yet.** Packers are standalone programs that read their sources, and the build server
    is reached only through RPC; the design comes once the architecture is in ([plan_editor_split.md](plan_editor_split.md), Open questions).
- **Interactive in the view** (gizmo drags, and later other viewport tools):
  - The view applies the transform at once, as a local opinion at the top of its stack, and draws from it.
  - It sends the edit by entity id to the editor (`prim.setTransform`, with the entity id and the new local transform).
  - The editor authors it into the edit layer with undo, so it gets saved with the layer.
  - When the packed result of that edit arrives as a delta, the view drops its local opinion. The picture doesn't change, because the packed value
    is the same. How the edit becomes packed data follows the authored path above, which is still open; the local opinion stays until then.
  - Drag frames can be coalesced: the view sends the latest value, and the editor authors on release or at a fixed rate.
  - No packing is on the per-frame path, and the build server has no special fast path.

### Deltas

The view's manifest holds:
- the scene version
- the layer stack, with each layer pack's version
- the variant selections
- the sub-packs and assets it holds

A delta has a base version and carries:
- layer pack replacements or opinion changes
- layer stack changes
- variant selection changes
- sub-packs and assets now required, by id

The view applies a delta only on a matching base version and fetches only what it lacks. On a mismatch, the server diffs against the manifest the view
reports and sends one catch-up delta. Local opinions from interactive edits sit above the stack until the matching repack arrives.

## Decisions

Locked unless marked open.

1. **Transport: TCP on loopback**, behind a small transport interface.
2. **Encoding: JSON-RPC 2.0 in length-prefixed frames, with optional binary attachments** for deltas and bulk data.
3. **Topology: direct connections plus discovery files (`_out/run/`), no broker.**
4. **Packs travel by reference locally.** They live in the build server's cache (`_out/<platform>/<config>/packs/`). Messages carry pack ids and paths, and the view
   memory-maps the packs.
5. **All packing happens in the build server, through build edges only.** There is no in-memory fast path. The editor and the view never pack.
   Interactive edits don't need packing on the per-frame path, because the view applies them locally first (Two edit paths).
6. **What the view knows:** layer packs, sub-packs, variant sets and components. The packer resolves the rest of USD composition.
7. **The build server is a mode of `ngen-build`: `ngen-build --serve`.** It uses the same graph code, and there is one tool to bootstrap.
8. **Windows split by what they are about.**
   - **In ngen-view: windows about its own live frame, or ones that need the viewport.** Frame Graph, Performance, Counters, Culling, Camera, Render
     Debug, and the Debug View legend with the cursor readout.
   - **In `ngen-introspect`: windows for capturing and examining data in depth, or seeing across processes.** Frame Debugger, Capture, GPU Scene,
     Memory, Events, pack and delta traffic, and a generic Records window.
   - Both sides read the same named records (step 4). The tool can show any record through the Records window without duplicating the view's
     windows.
9. **Library boundaries, enforced by `build.cpp`'s link graph.**
   - **Engine libraries** link no pxr: `rhi`, `renderer`, and a new runtime scene library that composes layer packs and variants and resolves the
     hierarchy.
   - **The USD packer** (`src/pack/usd/`, grown out of `USDRenderExtractor` and the loading half of `USDScene`) links into `ngen-build` only.
   - **USD editing** (`SceneUpdater`, undo, the editor windows) links into the editor only.
10. **Threading in ngen-view.** One I/O thread per endpoint. Deltas and pack loads are applied on the main thread at a fixed point in the frame, then
    reach the render thread through the existing snapshot and upload hand-off.
11. **Security: loopback only, no authentication.**

## Steps

### Step 1: RPC

Plan: [plan_rpc.md](plan_rpc.md).

- **Delivers:**
  - `src/rpc/`, in two layers:
    - a core with no dependencies beyond the standard library and header-only nlohmann/json (transport, framing, JSON-RPC, discovery files, calls
      in both directions), which the build server in `src/build/` also uses
    - an engine layer on top (method registry with parameter schemas, `rpc.describe`, dispatch onto the main and render threads)
  - `ngen-rpc`, the command-line client: `list`, `describe`, `call`
  - ngen-view as an endpoint:
    - today's session verbs as methods (`view.camera.set`, `view.screenshot`, `view.select`, `view.debugview`, `capture.*`, …)
    - today's dumps as methods (`introspect.get` over the existing writers)
    - `--script` runs through the same registry
- **Verification:**
  - `ngen-rpc call view introspect.get '{"name": "gpuscene"}'` on a running Sponza equals `dump-gpuscene`
  - `view.camera.set` plus `view.screenshot` equals the flag-driven PNG
  - `rpc.describe` lists every method with its schema
- **Gaps:**
  - request/response only, with no subscriptions or streams; step 2's `pack.ready` is a plain call back over the symmetric connection
  - bulk data is base64 in JSON
  - loopback only, no authentication

### Step 2: packs and the build server

- **Delivers:**
  - **The pack format:** a memory-mappable container of content-hashed chunks. It holds asset packs (mesh, texture, material), layer packs with variant
    alternatives, sub-packs, and a scene manifest (layer stack, variant sets and default selections, sub-pack list, scene settings). Components to
    start: transform with parent, mesh, material, light, camera, visibility.
  - **Pack rules and packers** ([plan_pack_rules.md](plan_pack_rules.md)):
    - `pack_rule` per asset type in `build.cpp`, and one packer program per type (shader, USD, texture, …)
    - stable path asset ids (USD's choice), with the content hash as the version
    - dependency records from the packers, and a reverse index
    - static pack targets, including the core pack ngen-view always loads
  - **Shaders become packed assets, loaded asynchronously and hot reloaded** (its own plan, `plan_async_shaders.md`).
    - The core pack holds the shaders every frame needs, and every other shader is requested at runtime.
    - A **renderer-level pipeline registry** creates pipelines: passes ask for a pipeline by description plus shader ids, not in `init()`. A
      pipeline whose shader hasn't arrived is pending, and its pass skips it or uses a fallback.
    - When a shader id gets a new version, whether from a request or from the server's watcher after a source edit, the registry rebuilds every
      pipeline using it. The old pipelines retire through the `DeletionQueue`.
    - The core pack only supplies first versions; a newer version from the server wins.
    - The RHI stays unchanged. RHI-level in-place pipeline rebuilds were the alternative; they would make the backend keep engine-level state.
  - **The build server** ([plan_build_server.md](plan_build_server.md)), the default path for every build:
    - `ngen-build` and `ngen-cli` start `ngen-build --serve` when none is running and send builds as requests; `--no-server` builds in-process
    - keeps the graph and build log resident and registers in discovery
    - takes `build.run` (targets and variant, with progress), `build.targets` and `build.variants`
    - `pack.request` for a scene or asset, answered at once. Then `pack.ready` (or `pack.failed`) arrives per asset as each finishes, so a scene
    loads fast and its sub-assets arrive as they're done
    - watches the sources of packed assets; the reverse index finds what a changed file affects, the server repacks it, and it sends `pack.ready`
      with the new version to the connected views
  - **The runtime scene library** (no pxr): layer stack and variant composition, sub-pack instancing, components and the transform hierarchy. It
    replaces `RenderWorld` as the renderer's input.
  - **ngen-view:**
    - asks the server for its scene's packs, or opens a pack directly for offline runs
    - exposes `scene.layers.set` (mute, unmute, order) and `scene.variants.set`, both applied without a repack
  - the engine libraries stop linking pxr
- **Verification:**
  - Sponza and three_cubes through the server render byte-identical screenshots to today
  - a warm open does no packing: no USD extraction or PNG decode, shown by events and timings
  - muting a layer through `scene.layers.set` renders the same image as the same stage packed with that layer muted
  - `scene.variants.set` on a test scene with a variant set renders the same image as the stage packed with that selection
  - the engine libraries link no `libusd_*` (link graph and `nm`)
  - hot reload: editing `shaders/lighting.frag` while Sponza runs changes the image within a second, with no restart and validation clean. A syntax
    error keeps the old shader and reports glslc's message as a `pack.failed` event
- **Gaps:**
  - the in-view editor windows still use USD in-process, so the ngen-view binary still links pxr until step 3
  - an edit in the in-view editor repacks the whole layer and reloads the scene: no deltas yet
  - non-transform changes still re-upload the GPU scene in full
  - the server reloads the whole graph after a `build.cpp` change
  - no physics or LOD components yet
  - nested variant sets are packed with their default selection only (see Open questions)
  - a shader edit that changes bindings or push constants can't be applied without reflection: the rebuild fails, the old version stays, and the
    reason is reported. Reflection in the shader packer's manifest (deferred from [plan_introspection.md](plan_introspection.md)) closes this.

### Step 3: editor split and deltas

Plan: [plan_editor_split.md](plan_editor_split.md).

- **Delivers:**
  - **`ngen-editor`** as its own process: the USD stage, layers, `SceneUpdater`, undo, and the editor windows (scene tree, properties, layers, undo,
    asset browser) move out of ngen-view. It exposes `scene.open`, `scene.save`, `prim.*`, `layer.*`, `variant.*` and `undo.*` for agents.
  - **Authored edits:** editor → changed layer → server repack → delta to every view.
  - **Interactive edits:**
    - gizmos in ngen-view, applied as local opinions
    - `prim.setTransform` by entity id to the editor, which authors into the edit layer with undo
    - the local opinion is dropped when the repacked layer arrives
  - **ngen-view:**
    - the manifest and versioning; `scene.applyDelta` and `scene.manifest`; resync on a version mismatch
    - incremental GPU scene updates: geometry pool allocation and freeing, material table and texture slot patches, instance adds and removes
    - picking returns entity ids to the editor
  - ngen-view links no pxr at all
- **Verification:**
  - a gizmo drag in the view moves the object every frame with no RPC round trip in the frame
  - after release the editor's layer holds the new transform, the repacked layer arrives, the local opinion is dropped, and the image doesn't change
    (byte-identical before and after)
  - `scene.save` writes the transform to the layer file
  - undo in the editor moves the object back in the view through a delta
  - swapping a texture in a layer shows the new texture after `pack.ready`, with the old one until then
  - a forced version mismatch resyncs to an image byte-identical to a fresh load
  - `nm`/`ldd` on ngen-view shows no `libusd_*`
- **Gaps:**
  - the viewport is a separate ngen-view window driven by the editor; it isn't embedded
  - one editor per scene, and conflicting edits from two views are last-writer-wins
  - no geometry pool compaction; long sessions fragment the pool
  - only transforms are interactive in the view; other viewport tools come later

### Step 4: introspection

It depends only on step 1, so it can run alongside steps 2–3.

- **Delivers:**
  - **One records and events model:**
    - named records with one field schema, and one `RenderThread` channel
    - `dump <name>` replaces the `dump-*` verbs
    - the obs bus trimmed to discrete events, with an in-process ring sink
  - **Streams:** subscriptions for events, record watches and profiler zones; binary attachments; backpressure without frame stalls; several clients
  - **`ngen-introspect`** with the examine-in-depth windows (Decision 8): Frame Debugger, Capture, GPU Scene, Memory, Events, pack and delta
    traffic, Records. Views auto-connect to an open tool.
  - **Those windows move out of ngen-view.** The live-frame windows stay: Frame Graph, Performance, Counters, Culling, Camera, Render Debug, Debug
    View legend and readout. They move onto the records model.
- **Verification:**
  - every moved window shows the same values in the tool as it did in-view for the same frame, checked window by window against a pre-step-4 build
  - the event stream over RPC equals `--obs-output`
  - a capture over RPC is byte-identical to the dump
  - a stalled client doesn't move the frame time
  - the moved windows' code is no longer linked into ngen-view (the link graph)
- **Gaps:**
  - no session recording
  - existing dump field names change where the schema renames them
  - without the tool running, the examine-in-depth windows aren't available; agents still have `dump` and RPC

## Cross-cutting

- **Naming: pack and packer throughout.** The pack cache, the `pack.*` methods and `src/pack/`.
- **ngen-cli:** the tool table gains `rpc` (step 1), `editor` (step 3) and `introspect` (step 4).
- **Agents:** `AGENTS.md` and the `run-headless` skill learn `ngen-rpc` in step 1, and the editor methods in step 3. Offline runs stay the default for
  reproducible verification. From step 2 on, an offline run needs the packs first, from a running server or a one-shot `ngen-build` pack target.
- **Documentation:**
  - `src/rpc/README.md`: the protocol, naming and threading
  - `src/pack/README.md`: the formats, the composition rule (layers and variants), stable ids, deltas and the two edit paths
  - `src/renderer/README.md` changes in steps 2–3
- **Observability of the plumbing:** RPC connections and errors, pack requests and times, deltas applied or rejected, resyncs, and local opinions
  pending are events. Queue depths and latencies are records.

## Known gaps (end state of this umbrella)

- No authentication or encryption. Loopback only, so no remote engine, devkit or pack bytes over the wire.
- No broker.
- No multi-editor semantics; conflicting interactive edits from several views are last-writer-wins.
- No session recording or replay.
- The viewport isn't embedded in the editor.
- No geometry pool compaction.
- No streaming of assets by distance or LOD. Sub-packs load whole.
- Physics and LOD components are named but not packed.
- Packing every variant grows pack size with the variant count.

## Open questions

- **Nested variant sets and variants that change a sub-pack's contents.** These can't always be packed as flat alternatives. Pack the combinations,
  pack only the default selection, or repack on switch? It is decided in the step 2 plan with a test scene.
- **Interactive tools beyond transforms** (material tweaks in the viewport, placement): do they follow the same local-opinion path? I lean yes, with
  the same rule: apply locally, author in the editor, drop on repack.
- **Coalescing drags:** does the editor author every received transform or only the final one, and what does undo record? I lean one undo entry per
  drag, authored on release, with intermediate values authored for other views at a fixed rate.

## Deferred / follow-ups

- **A broker.** Trigger: several engines per session, or tools on several machines.
- **Authentication and remote devices.** Trigger: ngen-view on another machine or a devkit.
- **Session recording (`.ngentrace`).** Trigger: inspecting a run after it ended.
- **Physics and LOD packers and components.** Trigger: the physics or LOD work starts.
- **Meshlets as a pack step.** Trigger: step 2 lands (`notes.md` ties meshlets to asset packing).
- **Shader reflection and pipeline state tables**, deferred from [plan_introspection.md](plan_introspection.md). The data goes in the shader
  packer's manifest, and the pipeline registry uses it to handle interface changes on hot reload. Trigger: `plan_async_shaders.md` lands.
